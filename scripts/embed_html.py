"""
PlatformIO build script to embed HTML into firmware.
Generates a C++ header with HTML data stored in PROGMEM.
"""

from __future__ import print_function
import gzip
import os
import re

Import("env")

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
        
        # Get environment name
        env_name = str(env['PIOENV'])
        
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

        # Define CAN configurations for all environments
        can_configs = {
            'esp32dev': {
                'title': 'CAN Bus Configuration',
                'can_fields': mcp_can_fields,
                'can_handlers': mcp_can_handlers
            },
            'esp32plus': {
                'title': 'CAN Bus Configuration',
                'can_fields': mcp_can_fields,
                'can_handlers': mcp_can_handlers
            },
            'esp32s3-MCP': {
                'title': 'CAN Bus Configuration',
                'can_fields': mcp_can_fields,
                'can_handlers': mcp_can_handlers
            },
            'xiao-esp32s3': {
                'title': 'CAN Bus Configuration',
                'can_fields': mcp_can_fields,
                'can_handlers': mcp_can_handlers
            },
            'esp32-ESPCAN': {
                'title': 'ESPCAN Configuration',
                'can_fields': '''<div class="form-row">
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
                  <label for="can_en_pin">ESPCAN Power/Enable Pin:</label>
                  <input type="number" id="can_en_pin" onchange="EnqueueUpdate('can_en_pin')" onkeypress="HandleEnter(event, 'can_en_pin')">
                </div>
              </div>''',
                'can_handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
          if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
          if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
          AckUpdate('can_rx_pin');
          AckUpdate('can_tx_pin');
          AckUpdate('can_en_pin');'''
            },
            'esp32s3-ESPCAN': {
                'title': 'ESPCAN Configuration',
                'can_fields': '''<div class="form-row">
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
                  <label for="can_en_pin">ESPCAN Power/Enable Pin:</label>
                  <input type="number" id="can_en_pin" onchange="EnqueueUpdate('can_en_pin')" onkeypress="HandleEnter(event, 'can_en_pin')">
                </div>
              </div>''',
                'can_handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
          if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
          if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
          AckUpdate('can_rx_pin');
          AckUpdate('can_tx_pin');
          AckUpdate('can_en_pin');'''
            },
            'esp32c3-ESPCAN': {
                'title': 'ESPCAN Configuration',
                'can_fields': '''<div class="form-row">
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
                  <label for="can_en_pin">ESPCAN Power/Enable Pin:</label>
                  <input type="number" id="can_en_pin" onchange="EnqueueUpdate('can_en_pin')" onkeypress="HandleEnter(event, 'can_en_pin')">
                </div>
              </div>''',
                'can_handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
          if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
          if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
          AckUpdate('can_rx_pin');
          AckUpdate('can_tx_pin');
          AckUpdate('can_en_pin');'''
            }
        }
        
        # Get config for this environment.
        #
        # can_configs is keyed by WIRING FAMILY (e.g. 'esp32s3-ESPCAN'), but
        # env['PIOENV'] is the full env name including its flash-size/PSRAM
        # suffix (e.g. 'esp32s3-ESPCAN-16mb-psram') - platformio.ini names
        # every env that way so a build can't be flashed onto the wrong
        # module. A bare env_name lookup here never matches any suffixed env
        # and silently falls back to the MCP2515 fields on every board,
        # which is invisible on MCP2515 boards (the fallback is correct for
        # them) and shows up as missing CAN pins on every ESPCAN board. Strip
        # the same suffix build-manifests.mjs strips before looking up.
        family = re.match(r'^(.*?)(?:-\d+mb)?(?:-psram)?$', env_name, re.IGNORECASE).group(1)
        config = can_configs.get(family, can_configs['esp32dev'])

        # Victron BLE. The radio only runs with PSRAM behind it - NimBLE alone
        # can tip a ~70KB internal-RAM board into the failed-allocation crash
        # documented in Diagnostics.h, and the firmware now refuses to start
        # the radio on such a board regardless of what the UI offers. The UI
        # is trimmed to match rather than left to discover that by trying:
        # every env here states PSRAM in its own name (see the header comment
        # in platformio.ini), so it is known at build time, not guessed.
        board_has_psram = env_name.endswith('-psram') or env_name == 'xiao-esp32s3'

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
        
        # The fan field used to be hidden on the C3, which has no MCPWM. The fan
        # now runs on LEDC, which every variant has, so it is shown everywhere.
        fan_field = '''<div class="form-group">
                <label for="fanpin"><span class="tip" data-tip="GPIO pin for PWM fan output. Set to 0 to disable.">FAN Pin:</span></label>
                <input type="number" id="fanpin" onchange="EnqueueUpdate('fanpin')" onkeypress="HandleEnter(event, 'fanpin')">
              </div>'''
        fan_handler = '''if(obj.hasOwnProperty('fanpin')) document.getElementById('fanpin').value=obj.fanpin;
          AckUpdate('fanpin');'''

        # Substitute placeholders
        html_content = html_content.replace('{{CAN_CONFIG_TITLE}}', config['title'])
        html_content = html_content.replace('{{CAN_CONFIG_FIELDS}}', config['can_fields'])
        html_content = html_content.replace('{{CAN_FIELD_HANDLERS}}', config['can_handlers'])
        html_content = html_content.replace('{{FAN_PIN_FIELD}}', fan_field)
        html_content = html_content.replace('{{FAN_PIN_HANDLER}}', fan_handler)
        html_content = html_content.replace('{{WIFI_TAB_LABEL}}', wifi_tab_label)
        html_content = html_content.replace('{{BLE_SHUNTSOURCE_OPTION}}', ble_shuntsource_option)
        html_content = html_content.replace('{{BLE_FALLBACKSOURCE_OPTION}}', ble_fallbacksource_option)
        html_content = html_content.replace('{{BLE_SHUNT_SECTION}}', ble_shunt_section)
        
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
