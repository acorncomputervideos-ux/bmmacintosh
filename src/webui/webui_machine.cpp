//
// webui_machine.cpp
//
// Modified for BMA800XL: the Atari 800XL's name, settings file and the
// file types atari800 can start, in place of BMC64's machines.
// Changed for BMCoco, BMApple2 and BMAtariST, and for BMMacintosh: the Mac
// running (the Mac Plus or the Mac II, each a kernel of its own), whose
// disk images can be started.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "webui.h"

#if BMC64_WEBUI

#include "webui_machine.h"

#include <stdio.h>

// The machine running (src/port/frontend.c), and the disk images its
// emulator takes (the web page's Disks, webui_disks.cpp).
extern "C" const char *bmmac_machine_text(void);
extern "C" const char *const *emu_run_types(void);
// The Mac Plus's settings are in bmmac.cfg; the Mac II's in macii.prefs.
extern "C" int emu_machine(void);

namespace {

WebUiMachine kMachine;

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

}  // namespace

const WebUiMachine *WebUiMachineGet(void) {
  kMachine.settings = emu_machine() == 1 ? "/macii.prefs" : "/bmmac.cfg";
  kMachine.run_types = emu_run_types();
  return &kMachine;
}

const char *WebUiMachineName(void) {
  return bmmac_machine_text();
}

boolean WebUiMachineCanRun(const char *ext) {
  for (const char *const *type = WebUiMachineGet()->run_types; *type != 0; type++) {
    const char *a = ext;
    const char *b = *type;
    while (*a != '\0' && Lower(*a) == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') return TRUE;
  }
  return FALSE;
}

int WebUiMachineCapsJson(char *out, unsigned size) {
  char run[256];
  unsigned used = 0;
  WebUiMachineGet();
  for (const char *const *type = kMachine.run_types; *type != 0; type++) {
    int n = snprintf(run + used, sizeof(run) - used, "%s\"%s\"",
                     used == 0 ? "" : ",", *type);
    if (n < 0 || (unsigned) n >= sizeof(run) - used) return -1;
    used += (unsigned) n;
  }
  run[used] = '\0';

  int length = snprintf(out, size,
                        "{\"settings\":\"%s\",\"run\":[%s]}",
                        kMachine.settings, run);
  if (length < 0 || (unsigned) length >= size) return -1;
  return length;
}

#endif  // BMC64_WEBUI
