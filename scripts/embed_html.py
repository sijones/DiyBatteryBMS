"""
PlatformIO build script to embed HTML into firmware.
Generates a C++ header with HTML data stored in PROGMEM.
"""

from __future__ import print_function
import gzip
import importlib.util
import os
import re

Import("env")

# ---------------------------------------------------------------------------
# Optional feature contributions to the page.
#
# The C++ half of an optional feature registers itself (see src/Features.h) and
# needs nothing added to a shared file. Its web UI needs the same, or the saving
# is only half made - a feature with settings would still mean editing this
# script and index.htm.template by hand, which are the two largest shared files
# there are.
#
# So a feature may also drop a module in scripts/sections/. Every .py file there
# is loaded and asked what it wants to contribute, and there is no list of them
# anywhere - adding one is adding a file, exactly as it is on the C++ side.
#
# A module may define any of:
#
#   applies(env_name, defines) -> bool
#       Whether this feature is in this build at all. Defaults to True.
#       `defines` is the env's -D flags as a set, so a module can gate on the
#       same symbol its C++ half does rather than on env-name string matching.
#
#   sections(env_name, defines) -> {anchor: html}
#       HTML or JS appended at a named EXT_ anchor in the template. Several
#       modules may contribute to one anchor; all are joined in filename order.
#
#   branding(env_name, defines) -> {placeholder: text}
#       Replaces a PRODUCT_ placeholder outright - the product name, wordmark
#       and mark. Unlike sections these do not accumulate, so exactly one module
#       in a build should set any given one; a second is reported rather than
#       silently winning.
#
# The anchors below are the extension points the template offers. They are
# deliberately few and generic: a feature fits itself to them rather than the
# template growing a placeholder per feature, which is the whole point.
EXT_ANCHORS = (
    'EXT_HEAD',               # extra <style>/<script> in <head>
    'EXT_HOME_PANELS',        # extra panels at the foot of the Home tab
    'EXT_SETTINGS_SECTIONS',  # extra sections at the foot of the Settings tab
    'EXT_WS_HANDLERS',        # extra JS inside the WebSocket update handler
)


def _env_defines(env):
    """The env's -D symbols as a set of bare names, without any =value part."""
    defines = set()
    for entry in env.get('CPPDEFINES', []):
        name = entry[0] if isinstance(entry, (list, tuple)) else entry
        defines.add(str(name))
    return defines


def _load_section_modules(project_dir):
    """Import every scripts/sections/*.py, in filename order.

    Loaded by path rather than by import name: this runs inside SCons, whose
    sys.path is not ours to extend, and the directory is frequently absent
    altogether (a build with no optional features), which is not an error.
    """
    sections_dir = os.path.join(project_dir, 'scripts', 'sections')
    if not os.path.isdir(sections_dir):
        return []

    modules = []
    for filename in sorted(os.listdir(sections_dir)):
        if not filename.endswith('.py') or filename.startswith('_'):
            continue
        path = os.path.join(sections_dir, filename)
        spec = importlib.util.spec_from_file_location(
            'bms_section_' + filename[:-3], path)
        if spec is None or spec.loader is None:
            continue
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        except Exception as exc:
            # A broken feature module must not take the build's page with it -
            # the firmware would still build and boot, and a silently blank
            # settings tab is far harder to diagnose than a printed name.
            print("[EMBED] Error loading section module {}: {}".format(filename, exc))
            continue
        modules.append((filename, module))
    return modules


def collect_feature_contributions(project_dir, env_name, defines):
    """Ask every section module what it contributes to this build.

    Returns (anchors, branding): anchors maps each EXT_ name to the joined HTML
    of every module that contributed to it, branding maps PRODUCT_ names to
    their replacement text.
    """
    anchors = dict((name, []) for name in EXT_ANCHORS)
    branding = {}
    branding_owner = {}

    for filename, module in _load_section_modules(project_dir):
        applies = getattr(module, 'applies', None)
        if callable(applies) and not applies(env_name, defines):
            continue

        provide = getattr(module, 'sections', None)
        if callable(provide):
            for anchor, html in (provide(env_name, defines) or {}).items():
                if anchor not in anchors:
                    print("[EMBED] {}: unknown anchor '{}' - ignored".format(filename, anchor))
                    continue
                anchors[anchor].append(html)

        brand = getattr(module, 'branding', None)
        if callable(brand):
            for key, text in (brand(env_name, defines) or {}).items():
                if key in branding_owner:
                    print("[EMBED] {}: '{}' already set by {} - overriding".format(
                        filename, key, branding_owner[key]))
                branding[key] = text
                branding_owner[key] = filename

    joined = dict((name, '\n'.join(parts)) for name, parts in anchors.items())
    active = [name for name, parts in anchors.items() if parts]
    if active or branding:
        print("[EMBED] Feature contributions: {}".format(
            ', '.join(sorted(active + list(branding.keys()))) or 'branding only'))
    return joined, branding

def generate_embedded_html(source, target, env):
    """Generate compressed HTML header file from templates."""
    try:
        project_dir = env['PROJECT_DIR']
        data_dir = os.path.join(project_dir, 'data')
        include_dir = os.path.join(project_dir, 'src')
        
        # Read and process index.htm template
        index_template = os.path.join(data_dir, 'index.htm.template')
        if not os.path.exists(index_template):
            print("[EMBED] Error: index.htm.template not found")
            return
        
        with open(index_template, 'r', encoding='utf-8') as f:
            html_content = f.read()
        
        # Get environment name and the -D symbols this build actually compiles with
        env_name = str(env['PIOENV'])
        defines = _env_defines(env)

        # MCP2515 CAN controller fields (SPI CS pin + crystal speed selector).
        # Shared by all MCP2515-based environments.
        mcp_can_fields = '''<div class="form-group">
                <label for="canbuscspin">CAN CS Pin:</label>
                <input type="number" id="canbuscspin" onchange="EnqueueUpdate('canbuscspin')" onkeypress="HandleEnter(event, 'canbuscspin')">
              </div>
              <div class="checkbox-row">
                <input type="checkbox" id="can16mhz" onchange="SendJSONUpdate('can16mhz')">
                <label for="can16mhz"><span class="tip" data-tip="Enable if your MCP2515 module uses a 16 MHz crystal. Leave unchecked for the common 8 MHz modules.">MCP2515 16 MHz Crystal</span></label>
              </div>'''
        mcp_can_handlers = '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
          if(obj.hasOwnProperty('can16mhz')) document.getElementById('can16mhz').checked=obj.can16mhz;
          AckUpdate('canbuscspin');
          AckUpdate('can16mhz');'''

        # Built-in CAN controller (TWAI) fields. Shared by every ESPCAN build -
        # classic ESP32 and S3 alike, which is all one wiring question: the pins
        # are plain number fields with nothing board-specific baked in.
        espcan_can_fields = '''<div class="form-row">
                <div class="form-group">
                  <label for="can_rx_pin">ESPCAN RX Pin:</label>
                  <input type="number" id="can_rx_pin" onchange="EnqueueUpdate('can_rx_pin')" onkeypress="HandleEnter(event, 'can_rx_pin')">
                </div>
                <div class="form-group">
                  <label for="can_tx_pin">ESPCAN TX Pin:</label>
                  <input type="number" id="can_tx_pin" onchange="EnqueueUpdate('can_tx_pin')" onkeypress="HandleEnter(event, 'can_tx_pin')">
                </div>
              </div>
              <div class="form-row full">
                <div class="form-group">
                  <label for="can_en_pin"><span class="tip" data-tip="Optional. GPIO that powers or enables the CAN transceiver, if yours has one - many do not, and the transceiver is simply always on. Leave blank if there is no enable line; the firmware then drives nothing and starts CAN on TX/RX alone.">ESPCAN Power/Enable Pin (optional):</span></label>
                  <input type="number" id="can_en_pin" placeholder="Not used" onchange="EnqueueUpdate('can_en_pin')" onkeypress="HandleEnter(event, 'can_en_pin')">
                </div>
              </div>'''
        espcan_can_handlers = '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
          if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
          if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
          AckUpdate('can_rx_pin');
          AckUpdate('can_tx_pin');
          AckUpdate('can_en_pin');'''

        # Which CAN controller this build talks to, asked of the build itself.
        #
        # This used to be a table keyed by wiring family, looked up with the env
        # name stripped of its -Nmb/-psram suffix, defaulting to the MCP2515
        # fields for anything unrecognised. Two things were wrong with that. The
        # default is silent and wrong in only one direction - an ESPCAN board
        # that misses gets a page with no CAN pins on it at all and no way to
        # start CAN, which is how a bare-env_name lookup was found in the first
        # place - and the table had to be extended by hand for every new env,
        # including ones this file never sees (envs/*.ini, see platformio.ini).
        #
        # ESPCAN is the symbol the firmware's own #ifdefs switch on: main.cpp's
        # CAN startup, buildDataDoc()'s payload, PIN_ROLES, the WS write
        # handlers. Reading the same symbol here means the page and the
        # firmware cannot disagree about which controller this build has -
        # whatever the env ends up being called.
        espcan = 'ESPCAN' in defines
        if espcan:
            config = {'can_fields': espcan_can_fields, 'can_handlers': espcan_can_handlers}
        else:
            config = {'can_fields': mcp_can_fields, 'can_handlers': mcp_can_handlers}
        print("[EMBED] CAN settings block: {}".format('ESPCAN (TWAI)' if espcan else 'MCP2515'))

        # Victron BLE. The radio only runs with PSRAM behind it - NimBLE alone
        # can tip a ~70KB internal-RAM board into the failed-allocation crash
        # documented in Diagnostics.h, and the firmware now refuses to start
        # the radio on such a board regardless of what the UI offers. The UI
        # is trimmed to match rather than left to discover that by trying.
        #
        # Read off the build's own flag rather than the env name, for the reason
        # the CAN block above is: the name says -psram only where there is a
        # pair of envs to tell apart, and the fixed single-SKU boards (XIAO,
        # Waveshare) have PSRAM without saying so in their name. The flag is
        # what NimBLE is linked against and what VictronBLE::Begin() compiles
        # against, so it is the thing the page has to agree with.
        board_has_psram = 'BOARD_HAS_PSRAM' in defines

        if board_has_psram:
            wifi_tab_label = 'WiFi/BLE/MQTT'
            ble_shuntsource_option = '<option value="1">Prefer BLE (Bluetooth)</option>'
            ble_fallbacksource_option = '<option value="1">BLE (Bluetooth)</option>'
            ble_shunt_section = '''<div class="section-title">Victron BLE Shunt</div>
        <!-- Always shown, so the shunt can be paired and tested before BLE is
             made the preferred source over in Settings > Victron Configuration.
             bleSourceHint says which way that preference is currently set. -->
        <div class="form-row">
          <div id="bleSourceHint" class="field-error"></div>
        </div>

        <div class="form-row">
          <div class="form-group">
            <label><span class="tip" data-tip="Listens for nearby Victron devices for 20 seconds. The advert header is not encrypted, so devices are found before any key is entered. A shunt at the edge of range only gets an advert through every half minute or so, which is why the wait is long.">Find your shunt:</span></label>
            <button id="bleScanBtn" onclick="startBleScan()">Scan for Victron devices</button>
            <div id="bleScanStatus" class="field-error"></div>
            <div id="bleFoundList"></div>
          </div>
        </div>

        <div class="form-row">
          <div class="form-group">
            <label for="blemac"><span class="tip" data-tip="Bluetooth address of your SmartShunt. Filled in by the Scan button, or type it as aa:bb:cc:dd:ee:ff.">Device Address:</span></label>
            <input type="textbox" id="blemac" placeholder="aa:bb:cc:dd:ee:ff" onchange="EnqueueUpdate('blemac')" onkeypress="HandleEnter(event, 'blemac')">
          </div>
          <div class="form-group">
            <label for="blekey"><span class="tip" data-tip="32 hex characters, from VictronConnect: connect to the shunt, then Settings > Product info > Instant readout via Bluetooth > Show. This is the only thing protecting the shunt's broadcasts, so it is stored but never sent back to the browser.">Encryption Key:</span></label>
            <div class="password-wrap">
              <input type="password" id="blekey" placeholder="32 hex characters" onchange="EnqueueUpdate('blekey')" onkeypress="HandleEnter(event, 'blekey')">
              <button class="password-toggle" onclick="togglePassword('blekey')">Show</button>
            </div>
            <div id="bleKeyState" class="field-error"></div>
          </div>
        </div>

        <div class="stat-grid">
          <div class="stat-box stat-status">
            <div class="stat-label">BLE Status</div>
            <div class="stat-value sm" id="bleStatus">--</div>
          </div>
          <div class="stat-box stat-status">
            <div class="stat-label">Adverts Decoded</div>
            <div class="stat-value sm" id="bleAdverts">--</div>
          </div>
          <div class="stat-box stat-status">
            <div class="stat-label">Rejected</div>
            <div class="stat-value sm" id="bleFailures">--</div>
          </div>
        </div>'''
        else:
            wifi_tab_label = 'WiFi/MQTT'
            ble_shuntsource_option = ''
            ble_fallbacksource_option = ''
            ble_shunt_section = ''

        # Per-core CPU headroom (Diagnostics.cpp's SampleCpuUsage()). S3-only,
        # and gated on the same BMS_S3 the firmware's own #if is gated on -
        # buildDataDoc() only puts cpuheadroom0/1 in the payload for a build
        # that defines it, so anything else here would be two boxes reading '--'
        # for the life of the device.
        board_is_s3 = 'BMS_S3' in defines
        if board_is_s3:
            cpu_headroom_section = '''<div class="stat-box stat-status">
            <div class="stat-label">CPU Headroom (Core 0)</div>
            <div class="stat-value sm" id="cpuHeadroom0">--</div>
          </div>
          <div class="stat-box stat-status">
            <div class="stat-label">CPU Headroom (Core 1)</div>
            <div class="stat-value sm" id="cpuHeadroom1">--</div>
          </div>'''
            cpu_headroom_handler = '''if(obj.hasOwnProperty('cpuheadroom0')) {
        var e0=document.getElementById('cpuHeadroom0');
        if(e0) e0.textContent = obj.cpuheadroom0 >= 0 ? obj.cpuheadroom0.toFixed(0) + '%' : '--';
      }
      if(obj.hasOwnProperty('cpuheadroom1')) {
        var e1=document.getElementById('cpuHeadroom1');
        if(e1) e1.textContent = obj.cpuheadroom1 >= 0 ? obj.cpuheadroom1.toFixed(0) + '%' : '--';
      }'''
        else:
            cpu_headroom_section = ''
            cpu_headroom_handler = ''

        # The fan field used to be hidden on the C3, which has no MCPWM. The fan
        # now runs on LEDC, which every variant has, so it is shown everywhere.
        fan_field = '''<div class="form-group">
                <label for="fanpin"><span class="tip" data-tip="Optional. GPIO pin for PWM fan output. Leave blank if no fan is fitted - the pin is then left alone and nothing is driven.">FAN Pin (optional):</span></label>
                <input type="number" id="fanpin" placeholder="Not used" onchange="EnqueueUpdate('fanpin')" onkeypress="HandleEnter(event, 'fanpin')">
              </div>'''
        fan_handler = '''if(obj.hasOwnProperty('fanpin')) document.getElementById('fanpin').value=obj.fanpin;
          AckUpdate('fanpin');'''

        # Product identity. Named here rather than written through the template
        # so a build can be branded by contributing a branding() dict from one
        # section module, instead of by a diff across the largest file in the
        # project. These defaults are the project's own identity, so an ordinary
        # build produces exactly the page it always did.
        branding_defaults = {
            'PRODUCT_TITLE': 'DIY Battery BMS',
            # Set the way the device already names itself - the hostname, the
            # MQTT client id, the discovery node - rather than as a product
            # wordmark. A build that is a product may well want the other.
            'PRODUCT_WORDMARK': 'diy<span class="accent">-</span>battery<span class="accent">-</span>bms',
            'PRODUCT_FAVICON': "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Crect x='3' y='4' width='18' height='4.6' rx='2.3' fill='%2335c9d4'/%3E%3Crect x='3' y='10.2' width='12.5' height='4.6' rx='2.3' fill='%23e6edf5'/%3E%3Crect x='3' y='16.4' width='7' height='4.6' rx='2.3' fill='%23e6edf5' opacity='0.45'/%3E%3C/svg%3E",
            'PRODUCT_MARK_CSS': '''.brand-mark { width: 23px; height: 23px; flex: none; color: var(--text); }
    .brand-mark .lvl-full { fill: var(--accent); }
    .brand-mark .lvl      { fill: currentColor; }
    .brand-mark .lvl-low  { fill: currentColor; opacity: .45; }''',
            'PRODUCT_MARK_SVG': '''<rect class="lvl-full" x="3" y="4" width="18" height="4.6" rx="2.3"/><rect class="lvl" x="3" y="10.2" width="12.5" height="4.6" rx="2.3"/><rect class="lvl-low" x="3" y="16.4" width="7" height="4.6" rx="2.3"/>''',
        }

        # Optional feature modules, if any - see collect_feature_contributions().
        ext_anchors, ext_branding = collect_feature_contributions(
            project_dir, env_name, _env_defines(env))
        branding_values = dict(branding_defaults)
        branding_values.update(ext_branding)

        # Substitute placeholders
        html_content = html_content.replace('{{CAN_CONFIG_FIELDS}}', config['can_fields'])
        html_content = html_content.replace('{{CAN_FIELD_HANDLERS}}', config['can_handlers'])
        html_content = html_content.replace('{{FAN_PIN_FIELD}}', fan_field)
        html_content = html_content.replace('{{FAN_PIN_HANDLER}}', fan_handler)
        html_content = html_content.replace('{{WIFI_TAB_LABEL}}', wifi_tab_label)
        html_content = html_content.replace('{{BLE_SHUNTSOURCE_OPTION}}', ble_shuntsource_option)
        html_content = html_content.replace('{{BLE_FALLBACKSOURCE_OPTION}}', ble_fallbacksource_option)
        html_content = html_content.replace('{{BLE_SHUNT_SECTION}}', ble_shunt_section)
        html_content = html_content.replace('{{CPU_HEADROOM_SECTION}}', cpu_headroom_section)
        html_content = html_content.replace('{{CPU_HEADROOM_HANDLER}}', cpu_headroom_handler)

        # Product identity, then whatever optional features contributed. Both
        # resolve to '' when nothing supplied them, so every placeholder is
        # always consumed and no build can ship a page with a literal {{...}}
        # showing in it.
        for key, text in branding_values.items():
            html_content = html_content.replace('{{' + key + '}}', text)
        for anchor in EXT_ANCHORS:
            html_content = html_content.replace('{{' + anchor + '}}', ext_anchors.get(anchor, ''))

        # Anything left is a placeholder the template asks for and nothing
        # answers - a typo in a name, or a section removed without its anchor.
        # Left in place it would render as literal braces on the page, so it is
        # named here instead, where a build log will show it.
        leftover = sorted(set(re.findall(r'\{\{([A-Z0-9_]+)\}\}', html_content)))
        if leftover:
            print("[EMBED] Warning: unsubstituted placeholder(s): {}".format(', '.join(leftover)))

        # Gzip the HTML - served with Content-Encoding: gzip, decompressed by the browser.
        # mtime=0 keeps output byte-identical between builds so the header only changes
        # when the HTML actually changes.
        raw_bytes = html_content.encode('utf-8')
        html_bytes = gzip.compress(raw_bytes, 9, mtime=0)

        # Generate C++ header
        header_content = '''// Auto-generated embedded HTML header
// DO NOT EDIT - Generated by embed_html.py
#pragma once
#include <stdint.h>

// Gzip-compressed HTML stored in PROGMEM.
// Must be served with a "Content-Encoding: gzip" header.
const uint8_t EMBEDDED_HTML[] PROGMEM = {
'''
        
        # Format as hex bytes with ASCII representation
        chars_per_line = 32
        for i, byte in enumerate(html_bytes):
            if i % chars_per_line == 0:
                header_content += '  '
            # Use byte value directly for non-printing characters
            byte_val = byte if isinstance(byte, int) else ord(byte)
            header_content += '0x{:02x},'.format(byte_val)
            if (i + 1) % chars_per_line == 0:
                header_content += '\n'
        
        header_content += '\n};\n'
        header_content += 'const uint32_t EMBEDDED_HTML_LEN = {};\n'.format(len(html_bytes))
        
        # Write header file
        header_path = os.path.join(include_dir, 'embedded_html.h')
        with open(header_path, 'w', encoding='utf-8') as f:
            f.write(header_content)
        
        print("[EMBED] Generated %s" % header_path)
        print("[EMBED] HTML size: %d bytes raw -> %d bytes gzip (%.1f%% smaller)"
              % (len(raw_bytes), len(html_bytes),
                 100.0 * (len(raw_bytes) - len(html_bytes)) / len(raw_bytes)))
        
    except Exception as e:
        print("[EMBED] Error: %s" % str(e))
        import traceback
        traceback.print_exc()

# Hook into build process
try:
    env.AddPreAction("$BUILD_DIR/src/main.cpp.o", generate_embedded_html)

    # The pre-action above only fires when main.cpp.o is actually rebuilt, and SCons
    # has no idea the HTML template feeds into it. Without these dependencies, editing
    # only index.htm.template produces a successful build containing the OLD page -
    # no warning, no [EMBED] line, byte-identical firmware. Declare them so a template
    # (or generator) change forces regeneration.
    _project_dir = env['PROJECT_DIR']
    _deps = [
        os.path.join(_project_dir, 'data', 'index.htm.template'),
        os.path.join(_project_dir, 'scripts', 'embed_html.py'),
    ]
    for _dep in _deps:
        if os.path.exists(_dep):
            env.Depends("$BUILD_DIR/src/main.cpp.o", _dep)
        else:
            print("[EMBED] Warning: dependency not found, skipping: %s" % _dep)
except Exception as e:
    print("[EMBED] Error registering build hook: %s" % str(e))
    import traceback
    traceback.print_exc()
