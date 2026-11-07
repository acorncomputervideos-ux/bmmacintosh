/*
 * menu.c - the BMMacintosh menu.
 *
 * F12, a gamepad's Home button or Select+Start open it. At the top, the
 * emulated Mac's own items (disks, ROM, memory: emu_menu_items() in
 * src/port/ii/ or src/port/plus/); then switching to the other Mac
 * (machine.c), BMMacintosh's settings (video, sound, network, kept in
 * bmmac.cfg), and power.
 *
 * From BMCoco's, BMApple2's and BMAtariST's menu.c.
 *
 * This file is part of BMMacintosh, a bare metal 68k Macintosh emulator for
 * the Raspberry Pi. See LICENSE for licensing details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../host.h"
#include "bmmac.h"
#include "osd.h"

#define WPA_CONFIG "wpa_supplicant.conf"

/* Something changed that only takes effect after a reboot. */
static int reboot_needed;

/* Where the file browser was last ("" until it's first used). */
static char browse_dir[256];

/* ---- Helpers ------------------------------------------------------------------ */

static void save_setting(const char *name, const char *value)
{
	if (!bmmac_save_setting(name, value))
		osd_message("Cannot save bmmac.cfg", TRUE);
}

static void save_setting_int(const char *name, int value)
{
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	save_setting(name, text);
}

static int setting_on(const char *name, int default_value)
{
	return bmmac_setting_on(name, default_value);
}

unsigned bmmac_check(int on)
{
	return OSD_ITEM_CHECK | (on ? OSD_ITEM_CHECKED : 0);
}

static unsigned check(int on)
{
	return bmmac_check(on);
}

void bmmac_reboot_needed(void)
{
	reboot_needed = TRUE;
}

static void power(int off)
{
	bmmac_request_power(off);
	bmmac_idle();
}

const char *bmmac_base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

int bmmac_browse(const char *title, const char *dir, const char *const exts[],
                 char *path, size_t size)
{
	if (browse_dir[0] == '\0')
		bmmac_card_path(browse_dir, sizeof browse_dir, dir, "");
	return osd_browse(title, browse_dir, sizeof browse_dir, exts, path, size);
}

/* ---- Machine ------------------------------------------------------------------- */

static const char *const machine_names[BMMAC_MACHINES] = {
	"Mac Plus (umac)", "Mac II and Quadra (Basilisk II)"
};

static void machine_menu(void)
{
	int sel = 0;
	for (;;) {
		int m, chosen;
		struct osd_item items[] = {
			{"Running", machine_names[emu_machine()], OSD_ITEM_LABEL, -1},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Mac Plus", NULL, check(emu_machine() == BMMAC_PLUS), BMMAC_PLUS},
			{"Mac II and Quadra", NULL, check(emu_machine() == BMMAC_II), BMMAC_II},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Each is a kernel of its own: switching restarts the Pi.", NULL,
			 OSD_ITEM_LABEL, -1},
			{"The Mac Plus runs on every Pi, with a Mac Plus ROM; the", NULL,
			 OSD_ITEM_LABEL, -1},
			{"Mac II and Quadra need a Pi 4 or 5, and a IIci's or a", NULL,
			 OSD_ITEM_LABEL, -1},
			{"Quadra's ROM (roms/ROMS.txt).", NULL, OSD_ITEM_LABEL, -1},
		};
		chosen = osd_menu("Machine", items, sizeof items / sizeof items[0], &sel);
		if (chosen < 0)
			return;
		m = chosen;
		if (m == emu_machine())
			continue;
		if (!bmmac_machine_available(m)) {
			osd_message("The Mac II and Quadra need a Raspberry Pi 4 or 5.", TRUE);
			continue;
		}
		if (!osd_confirm("Switch Machine", m == BMMAC_II
		                 ? "Restart the Pi as a Mac II / Quadra?"
		                 : "Restart the Pi as a Mac Plus?"))
			continue;
		emu_sync_disks();
		if (!bmmac_switch_machine(m)) {
			osd_message("Cannot change config.txt on the SD card.", TRUE);
			continue;
		}
		power(0);
		return;
	}
}

/* ---- Video -------------------------------------------------------------- */

static const char *vsync_mode(void)
{
	const char *mode = host_option("vsync", "auto");
	if (strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0)
		return "On";
	if (strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0)
		return "Off";
	return bmmac_vsync_locked() ? "Auto (locked)" : "Auto (timer)";
}

/* The picture's height as a percentage of the screen's (the scale option):
   less for a TV that cuts off the edges (overscan). From BMCoco. */
static int screen_size(void)
{
	double scale = host_option_double("scale", 1.0);
	if (scale <= 0.1 || scale > 1.0)
		scale = 1.0;
	return (int) (scale * 100.0 + 0.5);
}

static void choose_screen_size(void)
{
	static const char *const choices[] = {
		"100%: fill the screen", "95%", "90%", "85%", "80%", NULL
	};
	static const int sizes[] = {100, 95, 90, 85, 80};
	int size = screen_size(), current = 0, k, i;
	char text[16];
	for (k = 1; k < 5; k++) {
		int d = sizes[k] - size, best = sizes[current] - size;
		if ((d < 0 ? -d : d) < (best < 0 ? -best : best))
			current = k;
	}
	i = osd_choose("Screen Size", choices, current);
	if (i < 0)
		return;
	snprintf(text, sizeof text, "%d.%02d", sizes[i] / 100, sizes[i] % 100);
	save_setting("scale", text);
	bmmac_video_settings_changed();
}

static void video_menu(void)
{
	enum { V_SIZE, V_VSYNC, V_RESOLUTION, V_SHADER, V_CURVATURE, V_SMOOTH,
	       V_ACTIVITY };
	/* The Pi 5 only: its firmware ignores hdmi_mode (from BMA800XL). */
	static const char *const resolutions[] = {"auto", "1920x1080", "1280x720", NULL};
	static const char *const resolution_names[] = {"Auto", "1920x1080", "1280x720", NULL};
	int sel = 0;
	for (;;) {
		const char *res = host_option("resolution", "auto");
		int res_index = 0, n_items, k;
		char size_text[8];
		snprintf(size_text, sizeof size_text, "%d%%", screen_size());
		for (k = 0; resolutions[k] != NULL; k++)
			if (strcmp(res, resolutions[k]) == 0)
				res_index = k;
		struct osd_item items[] = {
			{"Screen Size", size_text, OSD_ITEM_SUBMENU, V_SIZE},
			{"Frame Lock", vsync_mode(), OSD_ITEM_SUBMENU, V_VSYNC},
			{"Resolution", resolution_names[res_index], OSD_ITEM_SUBMENU, V_RESOLUTION},
			{"CRT Effect (reboot)", NULL, check(setting_on("crt_shader", FALSE)), V_SHADER},
			{"CRT Curvature (reboot)", NULL, check(setting_on("crt_curvature", FALSE)), V_CURVATURE},
			{"Smooth Scaling (reboot)", NULL, check(setting_on("smooth", FALSE)), V_SMOOTH},
			{"Show Disk Activity", NULL, check(bmmac_disk_activity()), V_ACTIVITY},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"If your TV cuts off the edges, make", NULL, OSD_ITEM_LABEL, -1},
			{"the Screen Size smaller.", NULL, OSD_ITEM_LABEL, -1},
		};
		int chosen, i;
		n_items = sizeof(items) / sizeof(items[0]);
		if (!host_display_resolution_choice()) {
			/* Resolution is only for the Pi 5 (with video_driver=kms). */
			for (k = 2; k < n_items - 1; k++)
				items[k] = items[k + 1];
			n_items--;
		}
		chosen = osd_menu("Video", items, n_items, &sel);
		switch (chosen) {
		case V_SIZE:
			choose_screen_size();
			break;
		case V_ACTIVITY:
			save_setting("disk_activity", bmmac_disk_activity() ? "0" : "1");
			bmmac_set_disk_activity(!bmmac_disk_activity());
			break;
		case V_RESOLUTION:
			i = osd_choose("Resolution", resolution_names, res_index);
			if (i >= 0 && i != res_index) {
				save_setting("resolution", resolutions[i]);
				bmmac_display_mode_changed();
			}
			break;
		case V_CURVATURE:
			save_setting("crt_curvature", setting_on("crt_curvature", FALSE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		case V_VSYNC: {
			static const char *const choices[] = {
				"Automatic", "On: follow the display", "Off: keep exact time", NULL
			};
			static const char *const values[] = {"auto", "on", "off"};
			const char *mode = host_option("vsync", "auto");
			int current = strcmp(mode, "on") == 0 ? 1 : strcmp(mode, "off") == 0 ? 2 : 0;
			i = osd_choose("Frame Lock", choices, current);
			if (i >= 0) {
				save_setting("vsync", values[i]);
				bmmac_video_settings_changed();
			}
			break;
		}
		case V_SHADER:
			save_setting("crt_shader", setting_on("crt_shader", FALSE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		case V_SMOOTH:
			save_setting("smooth", setting_on("smooth", FALSE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		default:
			return;
		}
	}
}

/* ---- Sound ---------------------------------------------------------------- */

/* ---- Mouse ------------------------------------------------------------------ */

/* The USB mouse's speed on the Mac (frontend.c's bmmac_mouse_tick()). A
   modern mouse counts 800 or more steps an inch, where the Macs' counted
   about 100: the slower ones bring it nearer, with Mac OS's own Mouse
   control panel for the rest. */
static const char *const mouse_names[] = {
	"Slowest (1/4)", "Slower (1/2)", "Slow (3/4)", "Normal", "Fast (1.5x)", "Faster (2x)", NULL
};
static const char *const mouse_values[] = {"0.25", "0.5", "0.75", "1", "1.5", "2"};

static int mouse_speed_index(void)
{
	double speed = host_option_double("mouse_speed", 1.0);
	int i, best = 3;
	for (i = 0; i < 6; i++) {
		double v = atof(mouse_values[i]);
		if (speed > v - 0.01 && speed < v + 0.01)
			best = i;
	}
	return best;
}

static void mouse_menu(void)
{
	int i = osd_choose("Mouse Speed", mouse_names, mouse_speed_index());
	if (i >= 0)
		save_setting("mouse_speed", mouse_values[i]);
}

static void sound_menu(void)
{
	enum { S_VOLUME, S_OUTPUT };
	static const char *const volumes[] = {
		"100%", "90%", "80%", "70%", "60%", "50%", "40%", "30%", "20%", "10%", "Off", NULL
	};
	static const char *const outputs[] = {"auto", "hdmi", "analog"};
	static const char *const output_names[] = {"Automatic", "HDMI", "Headphone jack", NULL};
	int sel = 0;
	for (;;) {
		const char *out = host_option("audio_out", "auto");
		int out_index = strcmp(out, "hdmi") == 0 ? 1
		                : strcmp(out, "analog") == 0 || strcmp(out, "headphones") == 0 ? 2 : 0;
		char volume[8];
		struct osd_item items[] = {
			{"Volume", volume, OSD_ITEM_SUBMENU, S_VOLUME},
			{"Output (reboot)", output_names[out_index], OSD_ITEM_SUBMENU, S_OUTPUT},
		};
		int chosen, i;
		if (bmmac_volume() == 0)
			snprintf(volume, sizeof volume, "Off");
		else
			snprintf(volume, sizeof volume, "%d%%", bmmac_volume());
		chosen = osd_menu("Sound", items, sizeof(items) / sizeof(items[0]), &sel);
		if (chosen == S_VOLUME) {
			i = osd_choose("Volume", volumes, (100 - bmmac_volume() + 5) / 10);
			if (i >= 0)
				bmmac_set_volume(100 - i * 10);
			continue;
		}
		if (chosen != S_OUTPUT)
			return;
		i = osd_choose("Sound Output", output_names, out_index);
		if (i >= 0 && i != out_index) {
			save_setting("audio_out", outputs[i]);
			reboot_needed = TRUE;
		}
	}
}

/* ---- Network --------------------------------------------------------------- */

static char wifi_ssid[33], wifi_psk[64], wifi_country[3];

static void wpa_path(char *path, size_t size)
{
	const char *root = bmmac_sd_root();
	size_t len = strlen(root);
	snprintf(path, size, "%s%s%s", root, len > 0 && root[len - 1] == '/' ? "" : "/", WPA_CONFIG);
}

static void copy_quoted(char *dst, size_t size, const char *src)
{
	const char *end;
	if (*src == '"')
		src++;
	end = strchr(src, '"');
	if (end == NULL)
		end = src + strcspn(src, "\r\n");
	if ((size_t) (end - src) >= size)
		end = src + size - 1;
	memcpy(dst, src, end - src);
	dst[end - src] = '\0';
}

static void read_wifi_config(void)
{
	char path[256], line[160];
	FILE *f;
	wifi_ssid[0] = wifi_psk[0] = '\0';
	strcpy(wifi_country, "US");
	wpa_path(path, sizeof(path));
	f = fopen(path, "r");
	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *p = line + strspn(line, " \t");
		if (strncmp(p, "country=", 8) == 0)
			copy_quoted(wifi_country, sizeof(wifi_country), p + 8);
		else if (strncmp(p, "ssid=", 5) == 0 && wifi_ssid[0] == '\0')
			copy_quoted(wifi_ssid, sizeof(wifi_ssid), p + 5);
		else if (strncmp(p, "psk=", 4) == 0 && wifi_psk[0] == '\0')
			copy_quoted(wifi_psk, sizeof(wifi_psk), p + 4);
	}
	fclose(f);
}

static int write_wifi_config(void)
{
	char path[256];
	FILE *f;
	wpa_path(path, sizeof(path));
	f = fopen(path, "w");
	if (f == NULL)
		return FALSE;
	fprintf(f, "country=%s\n\nnetwork={\n\tssid=\"%s\"\n", wifi_country, wifi_ssid);
	if (wifi_psk[0] != '\0')
		fprintf(f, "\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n", wifi_psk);
	else
		fprintf(f, "\tkey_mgmt=NONE\n");
	fprintf(f, "}\n");
	return fclose(f) == 0;
}

static void edit_wifi(const char *title, char *value, size_t size, int secret)
{
	char text[64];
	snprintf(text, sizeof(text), "%s", value);
	if (osd_edit(title, text, size < sizeof(text) ? size : sizeof(text), secret)) {
		snprintf(value, size, "%s", text);
		if (write_wifi_config())
			reboot_needed = TRUE;
		else
			osd_message("Cannot write " WPA_CONFIG, TRUE);
	}
}

static int compare_signal(const void *a, const void *b)
{
	return ((const struct host_wifi_ap *) b)->signal - ((const struct host_wifi_ap *) a)->signal;
}

/* Returns 1 if a network was chosen, 2 if it also needs a password. */
static int scan_wifi(void)
{
	enum { MAX_APS = 20 };
	static struct host_wifi_ap aps[MAX_APS];
	static char signal[MAX_APS][16];
	struct osd_item items[MAX_APS];
	int n, i, sel = 0, chosen;

	osd_message("Scanning for Wi-Fi networks...", FALSE);
	n = host_wifi_scan(aps, MAX_APS);
	if (n < 0) {
		if (osd_confirm("Wi-Fi is Off", "Switch to Wi-Fi and reboot?")) {
			save_setting("network", "wifi");
			power(0);
		}
		return 0;
	}
	if (n == 0) {
		osd_message("No Wi-Fi networks found.", TRUE);
		return 0;
	}
	qsort(aps, n, sizeof(aps[0]), compare_signal);
	for (i = 0; i < n; i++) {
		snprintf(signal[i], sizeof(signal[i]), "%s%d", aps[i].secure ? "* " : "", aps[i].signal);
		items[i] = (struct osd_item){aps[i].ssid, signal[i], 0, i};
	}
	chosen = osd_menu("Choose a Wi-Fi Network", items, n, &sel);
	if (chosen < 0)
		return 0;
	if (!aps[chosen].secure || strcmp(wifi_ssid, aps[chosen].ssid) != 0)
		wifi_psk[0] = '\0';
	snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", aps[chosen].ssid);
	if (!write_wifi_config()) {
		osd_message("Cannot write " WPA_CONFIG, TRUE);
		return 0;
	}
	save_setting("network", "wifi");
	reboot_needed = TRUE;
	return aps[chosen].secure && wifi_psk[0] == '\0' ? 2 : 1;
}

static void network_menu(void)
{
	enum { N_DEVICE, N_SCAN, N_SSID, N_PASSWORD, N_COUNTRY, N_WEBUI, N_PIN, N_PAUSE, N_TIMEZONE,
	       N_REBOOT };
	static const char *const devices[] = {"off", "ethernet", "wifi", NULL};
	static const char *const device_names[] = {"Off", "Ethernet", "Wi-Fi", NULL};
	int sel = 0, first = TRUE;
	for (;;) {
		char status[64], tz[16], address[64], web[96], drive[96], mac_net[128], xfer[220];
		const char *net = host_option("network", "off");
		const char *pin = host_option("webui_pin", "");
		int device = strcmp(net, "ethernet") == 0 ? 1 : strcmp(net, "wifi") == 0 ? 2 : 0;
		int minutes = host_option_int("timezone", 0);
		int chosen, i;

		read_wifi_config();
		host_network_status(status, sizeof(status));
		/* Where the web page and the network drive are, once the Pi has
		   an address, and how the Mac II's own network is. */
		web[0] = drive[0] = mac_net[0] = '\0';
		if (setting_on("webui", TRUE) && host_network_address(address, sizeof(address))) {
			snprintf(web, sizeof(web), "Web page: http://%s/", address);
			snprintf(drive, sizeof(drive), "Network drive: http://%s/sd/", address);
		}
		xfer[0] = '\0';
		if (bmmac_last_transfer()[0])
			snprintf(xfer, sizeof(xfer), "Last: %.72s", bmmac_last_transfer());
		if (emu_network_text() != NULL)
			snprintf(mac_net, sizeof(mac_net), "The Mac's network: %s", emu_network_text());
		if (minutes == 0)
			strcpy(tz, "UTC");
		else
			snprintf(tz, sizeof(tz), "UTC%+d", minutes / 60);
		{
			struct osd_item items[] = {
				{status, NULL, OSD_ITEM_LABEL, -1},
				{web, NULL, OSD_ITEM_LABEL, -1},
				{drive, NULL, OSD_ITEM_LABEL, -1},
				{mac_net, NULL, OSD_ITEM_LABEL, -1},
				{xfer, NULL, OSD_ITEM_LABEL, -1},
				{"", NULL, OSD_ITEM_LABEL, -1},
				{"Network", device_names[device], OSD_ITEM_SUBMENU, N_DEVICE},
				{"Scan for Wi-Fi Networks", NULL, OSD_ITEM_SUBMENU, N_SCAN},
				{"Wi-Fi Network", wifi_ssid[0] ? wifi_ssid : "(none)", OSD_ITEM_SUBMENU, N_SSID},
				{"Wi-Fi Password", wifi_psk[0] ? "********" : "(none)", OSD_ITEM_SUBMENU, N_PASSWORD},
				{"Wi-Fi Country", wifi_country, OSD_ITEM_SUBMENU, N_COUNTRY},
				{"Web UI", NULL, check(setting_on("webui", TRUE)), N_WEBUI},
				{"Web UI PIN", pin[0] ? pin : "(none)", OSD_ITEM_SUBMENU, N_PIN},
				{"Pause the Mac During Transfers", NULL, check(setting_on("transfer_pause", TRUE)),
				 N_PAUSE},
				{"Time Zone", tz, OSD_ITEM_SUBMENU, N_TIMEZONE},
				{"", NULL, OSD_ITEM_LABEL, -1},
				{"Changes apply after a reboot.", NULL, OSD_ITEM_LABEL, -1},
				{"Reboot Now", NULL, 0, N_REBOOT},
			};
			/* The address and network lines only when there is something
			   to say. */
			struct osd_item shown[sizeof(items) / sizeof(items[0])];
			int n = 0;
			for (i = 0; i < (int) (sizeof(items) / sizeof(items[0])); i++) {
				const char *t = items[i].text;
				if ((t == web || t == drive || t == mac_net || t == xfer) && t[0] == '\0')
					continue;
				if (first && items[i].id == N_DEVICE)
					sel = n;
				shown[n++] = items[i];
			}
			first = FALSE;
			chosen = osd_menu("Network", shown, n, &sel);
		}
		switch (chosen) {
		case N_DEVICE:
			i = osd_choose("Network", device_names, device);
			if (i >= 0 && i != device) {
				save_setting("network", devices[i]);
				reboot_needed = TRUE;
			}
			break;
		case N_SCAN:
			if (scan_wifi() == 2) {
				wifi_psk[0] = '\0';
				edit_wifi("Wi-Fi Password", wifi_psk, sizeof(wifi_psk), TRUE);
			}
			break;
		case N_SSID:
			edit_wifi("Wi-Fi Network Name", wifi_ssid, sizeof(wifi_ssid), FALSE);
			break;
		case N_PASSWORD:
			wifi_psk[0] = '\0';
			edit_wifi("Wi-Fi Password (blank: none)", wifi_psk, sizeof(wifi_psk), TRUE);
			break;
		case N_COUNTRY:
			edit_wifi("Two Letter Country Code", wifi_country, sizeof(wifi_country), FALSE);
			break;
		case N_WEBUI:
			save_setting("webui", setting_on("webui", TRUE) ? "0" : "1");
			reboot_needed = TRUE;
			break;
		case N_PAUSE:
			/* Takes effect at once (frontend.c's transfer_pause()). */
			save_setting("transfer_pause", setting_on("transfer_pause", TRUE) ? "0" : "1");
			break;
		case N_PIN: {
			char text[9];
			snprintf(text, sizeof(text), "%s", pin);
			if (osd_edit("Web UI PIN (blank: none)", text, sizeof(text), FALSE)) {
				save_setting("webui_pin", text);
				reboot_needed = TRUE;
			}
			break;
		}
		case N_TIMEZONE: {
			static char labels[27][8];
			static const char *zones[28];
			int h, current = 12;
			for (h = -12; h <= 14; h++) {
				if (h == 0)
					strcpy(labels[h + 12], "UTC");
				else
					snprintf(labels[h + 12], sizeof(labels[0]), "UTC%+d", h);
				zones[h + 12] = labels[h + 12];
				if (h * 60 == minutes)
					current = h + 12;
			}
			zones[27] = NULL;
			h = osd_choose("Time Zone", zones, current);
			if (h >= 0 && h != current) {
				save_setting_int("timezone", (h - 12) * 60);
				reboot_needed = TRUE;
			}
			break;
		}
		case N_REBOOT:
			power(0);
			return;
		default:
			return;
		}
	}
}

/* ---- Information ----------------------------------------------------------- */

/* The frames' pace, as a percentage of the real Mac's. The Mac Plus's CPU
   is timed to the frames, so that is its speed too; the Mac II's
   (Basilisk II's) runs as many instructions as the Pi can between them,
   often several times a real Quadra's, so the percentage says only that
   it keeps up. */
static const char *speed_text(int long_form)
{
	static char text[96];
	if (emu_machine() == BMMAC_II)
		snprintf(text, sizeof text, long_form
		         ? "keeps up (%d%% of its frames), CPU at full speed"
		         : "Keeping up at %d%%; CPU at full speed.", bmmac_speed_percent());
	else
		snprintf(text, sizeof text, long_form
		         ? "%d%% of the real machine's"
		         : "Running at %d%% of the real Mac's speed.", bmmac_speed_percent());
	return text;
}

static void about(void)
{
	char text[1200], status[64];
	int w, h;
	host_display_size(&w, &h);
	host_network_status(status, sizeof(status));
	snprintf(text, sizeof(text),
	         "BMMacintosh " BMMAC_VERSION "\n"
	         "Bare metal 68k Macintosh emulator for the Raspberry Pi.\n\n"
	         "%s\n"
	         "Based on BMAtariST, BMApple2, BMCoco, BMA800XL and BMC64.\n\n"
	         "Machine: %s\n"
	         "Speed: %s\n"
	         "Display: %dx%d %.2f Hz, frames %s\n"
	         "Network: %s",
	         emu_credits(), emu_machine_text(), speed_text(TRUE), w, h,
	         bmmac_refresh_rate(),
	         bmmac_vsync_locked() ? "locked to the display" : "timer paced", status);
	osd_info("About BMMacintosh", text);
}

static void offer_reboot(void)
{
	if (osd_confirm("Reboot?", "Settings changed that need a reboot. Reboot now?"))
		power(0);
}

/* ---- Main menu ---------------------------------------------------------------- */

#define EMU_ITEMS 16

void bmmac_menu(void)
{
	enum { M_MACHINE = 1, M_VIDEO, M_SOUND, M_MOUSE, M_NETWORK, M_ABOUT, M_REBOOT, M_OFF };
	static int sel = 0;
	char title[48], speed[48];

	snprintf(title, sizeof(title), "BMMacintosh %s", BMMAC_VERSION);
	reboot_needed = FALSE;
	osd_open();
	for (;;) {
		struct osd_item items[EMU_ITEMS + 12];
		int n = emu_menu_items(items, EMU_ITEMS), chosen, leave = FALSE;
		const struct osd_item common[] = {
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Machine", emu_machine() == BMMAC_II ? "Mac II and Quadra" : "Mac Plus",
			 OSD_ITEM_SUBMENU, M_MACHINE},
			{"Video", NULL, OSD_ITEM_SUBMENU, M_VIDEO},
			{"Sound", NULL, OSD_ITEM_SUBMENU, M_SOUND},
			{"Mouse Speed", mouse_names[mouse_speed_index()], OSD_ITEM_SUBMENU, M_MOUSE},
			{"Network", NULL, OSD_ITEM_SUBMENU, M_NETWORK},
			{"About", NULL, OSD_ITEM_SUBMENU, M_ABOUT},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{"Reboot", NULL, 0, M_REBOOT},
			{"Shut Down", NULL, 0, M_OFF},
			{"", NULL, OSD_ITEM_LABEL, -1},
			{speed, NULL, OSD_ITEM_LABEL, -1},
		};
		memcpy(items + n, common, sizeof common);
		n += sizeof common / sizeof common[0];
		snprintf(speed, sizeof speed, "%s", speed_text(FALSE));
		chosen = osd_menu(title, items, n, &sel);

		switch (chosen) {
		case M_MACHINE: machine_menu(); break;
		case M_VIDEO: video_menu(); break;
		case M_SOUND: sound_menu(); break;
		case M_MOUSE: mouse_menu(); break;
		case M_NETWORK: network_menu(); break;
		case M_ABOUT: about(); break;
		case M_REBOOT:
			if (osd_confirm("Reboot", "Reboot BMMacintosh?"))
				power(0);
			break;
		case M_OFF:
			if (osd_confirm("Shut Down", "Shut down? It's then safe to switch off."))
				power(1);
			break;
		default:
			if (chosen >= 100)
				leave = emu_menu_choose(chosen);
			else
				leave = TRUE;
			break;
		}
		if (leave || osd_closing() || bmmac_pending_requests())
			break;
	}
	if (reboot_needed && !osd_closing())
		offer_reboot();
	osd_close();
}
