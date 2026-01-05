"""
PlatformIO build script to embed HTML into firmware.
Generates a C++ header with HTML data stored in PROGMEM.
"""

from __future__ import print_function
import os

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
        
        # Define CAN configurations for all environments
        can_configs = {
            'esp32dev': {
                'title': 'CAN Bus Configuration',
                'can_fields': '''<div class="form-group">
                <label for="canbuscspin">CAN CS Pin:</label>
                <input type="number" id="canbuscspin" onchange="EnqueueUpdate('canbuscspin')" onkeypress="HandleEnter(event, 'canbuscspin')">
              </div>''',
                'can_handlers': '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
          AckUpdate('canbuscspin');'''
            },
            'esp32plus': {
                'title': 'CAN Bus Configuration',
                'can_fields': '''<div class="form-group">
                <label for="canbuscspin">CAN CS Pin:</label>
                <input type="number" id="canbuscspin" onchange="EnqueueUpdate('canbuscspin')" onkeypress="HandleEnter(event, 'canbuscspin')">
              </div>''',
                'can_handlers': '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
          AckUpdate('canbuscspin');'''
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
        
        # Get config for this environment
        config = can_configs.get(env_name, can_configs['esp32dev'])
        
        # Determine if FAN is hidden (C3 only)
        hide_fan = 'ESPCAN_C3' in env.get('BUILD_FLAGS', '') or env_name == 'esp32c3-ESPCAN'
        
        fan_field = ''
        fan_handler = ''
        if not hide_fan:
            fan_field = '''<div class="form-group">
                <label for="fanpin">FAN Pin:</label>
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
        
        # Store HTML as-is (no compression - simpler and avoids zlib dependency)
        html_bytes = html_content.encode('utf-8')
        
        # Generate C++ header
        header_content = '''// Auto-generated embedded HTML header
// DO NOT EDIT - Generated by embed_html.py
#pragma once
#include <stdint.h>

// HTML data stored in PROGMEM
const char EMBEDDED_HTML[] PROGMEM = {
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
        print("[EMBED] HTML size: %d bytes" % len(html_bytes))
        
    except Exception as e:
        print("[EMBED] Error: %s" % str(e))
        import traceback
        traceback.print_exc()

# Hook into build process
try:
    env.AddPreAction("$BUILD_DIR/src/main.cpp.o", generate_embedded_html)
except Exception as e:
    print("[EMBED] Error registering build hook: %s" % str(e))
    import traceback
    traceback.print_exc()
