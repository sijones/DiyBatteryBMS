"""
PlatformIO build script to generate environment-specific index.htm files from template.
Runs during buildfs phase to substitute CAN configuration fields.

Place in scripts/ directory and add to platformio.ini:
  extra_scripts = scripts/build_html.py
"""

from __future__ import print_function
import os
import shutil

Import("env")

# Environment-specific CAN configurations
CAN_CONFIGS = {
    'esp32-ESPCAN': {
        'title': 'ESPCAN Configuration',
        'fields': '''<div class="form-row">
          <div class="form-group">
            <label for="can_rx_pin">ESPCAN RX Pin:</label>
            <input type="number" id="can_rx_pin" onblur="SendJSONUpdate('can_rx_pin')">
          </div>
          <div class="form-group">
            <label for="can_tx_pin">ESPCAN TX Pin:</label>
            <input type="number" id="can_tx_pin" onblur="SendJSONUpdate('can_tx_pin')">
          </div>
        </div>
        <div class="form-row full">
          <div class="form-group">
            <label for="can_en_pin">ESPCAN Power/Enable Pin:</label>
            <input type="number" id="can_en_pin" onblur="SendJSONUpdate('can_en_pin')">
          </div>
        </div>''',
        'handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
      if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
      if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
      '''
    },
    'esp32s3-ESPCAN': {
        'title': 'ESPCAN Configuration',
        'fields': '''<div class="form-row">
          <div class="form-group">
            <label for="can_rx_pin">ESPCAN RX Pin:</label>
            <input type="number" id="can_rx_pin" onblur="SendJSONUpdate('can_rx_pin')">
          </div>
          <div class="form-group">
            <label for="can_tx_pin">ESPCAN TX Pin:</label>
            <input type="number" id="can_tx_pin" onblur="SendJSONUpdate('can_tx_pin')">
          </div>
        </div>
        <div class="form-row full">
          <div class="form-group">
            <label for="can_en_pin">ESPCAN Power/Enable Pin:</label>
            <input type="number" id="can_en_pin" onblur="SendJSONUpdate('can_en_pin')">
          </div>
        </div>''',
        'handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
      if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
      if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
      '''
    },
    'esp32dev': {
        'title': 'CAN Bus Configuration',
        'fields': '''<div class="form-group">
            <label for="canbuscspin">CAN CS Pin:</label>
            <input type="number" id="canbuscspin" onblur="SendJSONUpdate('canbuscspin')">
          </div>''',
        'handlers': '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
      '''
    },
    'esp32plus': {
        'title': 'CAN Bus Configuration',
        'fields': '''<div class="form-group">
            <label for="canbuscspin">CAN CS Pin:</label>
            <input type="number" id="canbuscspin" onblur="SendJSONUpdate('canbuscspin')">
          </div>''',
        'handlers': '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
      '''
    },
    'esp32c3-ESPCAN': {
        'title': 'ESPCAN Configuration',
        'fields': '''<div class="form-row">
          <div class="form-group">
            <label for="can_rx_pin">ESPCAN RX Pin:</label>
            <input type="number" id="can_rx_pin" onblur="SendJSONUpdate('can_rx_pin')">
          </div>
          <div class="form-group">
            <label for="can_tx_pin">ESPCAN TX Pin:</label>
            <input type="number" id="can_tx_pin" onblur="SendJSONUpdate('can_tx_pin')">
          </div>
        </div>
        <div class="form-row full">
          <div class="form-group">
            <label for="can_en_pin">ESPCAN Power/Enable Pin:</label>
            <input type="number" id="can_en_pin" onblur="SendJSONUpdate('can_en_pin')">
          </div>
        </div>''',
        'handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
      if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
      if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
      ''',
        'hide_fan': True  # C3 doesn't support MCPWM/FAN
    }
}

def process_template(source, target, env):
    """Process the templates and generate the output HTML files."""
    try:
        env_name = str(env['PIOENV'])
        project_dir = env['PROJECT_DIR']
        # Write outputs to a single shared data directory
        output_dir = os.path.join(project_dir, 'data')
        
        # Ensure output directory exists
        os.makedirs(output_dir, exist_ok=True)
        
        # Get configuration for this environment
        config = CAN_CONFIGS.get(env_name, CAN_CONFIGS['esp32dev'])
        
        # FAN field configuration (hide on C3)
        fan_field = ''
        fan_handler = ''
        if not config.get('hide_fan', False):
            fan_field = '''<div class="form-group">
            <label for="fanpin">FAN Pin:</label>
            <input type="number" id="fanpin" onblur="SendJSONUpdate('fanpin')">
          </div>'''
            fan_handler = "if(obj.hasOwnProperty('fanpin')) document.getElementById('fanpin').value=obj.fanpin;"
        
        # Process index.htm
        index_template = os.path.join(project_dir, 'data', 'index.htm.template')
        index_output = os.path.join(output_dir, 'index.htm')
        
        with open(index_template, 'r', encoding='utf-8') as f:
            html_content = f.read()
        
        html_content = html_content.replace('{{CAN_CONFIG_TITLE}}', config['title'])
        html_content = html_content.replace('{{CAN_CONFIG_FIELDS}}', config['fields'])
        html_content = html_content.replace('{{CAN_FIELD_HANDLERS}}', config['handlers'])
        html_content = html_content.replace('{{FAN_PIN_FIELD}}', fan_field)
        html_content = html_content.replace('{{FAN_PIN_HANDLER}}', fan_handler)
        
        with open(index_output, 'w', encoding='utf-8') as f:
          f.write(html_content)
        
        print("[HTML] Generated %s for environment: %s" % (index_output, env_name))
        
        # Process index-ap.htm (AP mode)
        ap_template = os.path.join(project_dir, 'data', 'index-ap.htm.template')
        ap_output = os.path.join(output_dir, 'index-ap.htm')
        
        if os.path.exists(ap_template):
            with open(ap_template, 'r', encoding='utf-8') as f:
                ap_content = f.read()
            
            ap_content = ap_content.replace('{{CAN_CONFIG_TITLE}}', config['title'])
            ap_content = ap_content.replace('{{CAN_CONFIG_FIELDS}}', config['fields'])
            ap_content = ap_content.replace('{{CAN_FIELD_HANDLERS}}', config['handlers'])
            ap_content = ap_content.replace('{{FAN_PIN_FIELD}}', fan_field)
            ap_content = ap_content.replace('{{FAN_PIN_HANDLER}}', fan_handler)
            
            with open(ap_output, 'w', encoding='utf-8') as f:
              f.write(ap_content)
            
            print("[HTML] Generated %s for environment: %s" % (ap_output, env_name))
        else:
            print("[HTML] Warning: AP template not found at %s" % ap_template)
    except Exception as e:
        print("[HTML] Error: %s" % str(e))
        import traceback
        traceback.print_exc()


def prepare_filtered_data_dir(env):
  """Create a filtered data directory without template files and point buildfs to it."""
  project_dir = env['PROJECT_DIR']
  build_dir = env.subst('$BUILD_DIR')
  source_data = os.path.join(project_dir, 'data')
  filtered_dir = os.path.join(build_dir, 'littlefs_data_filtered')

  if os.path.exists(filtered_dir):
    shutil.rmtree(filtered_dir)

  for root, dirs, files in os.walk(source_data):
    rel_root = os.path.relpath(root, source_data)
    target_root = os.path.join(filtered_dir, rel_root) if rel_root != '.' else filtered_dir
    os.makedirs(target_root, exist_ok=True)

    for fname in files:
      if fname.endswith('.template'):
        continue  # skip templates from filesystem image
      src_path = os.path.join(root, fname)
      dst_path = os.path.join(target_root, fname)
      shutil.copy2(src_path, dst_path)

  # Point PlatformIO's data dir to the filtered copy for this build
  env.Replace(PROJECTDATA_DIR=filtered_dir)

# Register the action - run immediately when the script is loaded
def post_program_action(source, target, env):
  """Auto-trigger template generation."""
  process_template(None, None, env)

# Run as pre-action on compilation
env.AddPreAction("$BUILD_DIR/firmware.elf", process_template)
# Also run on littlefs build: generate HTML then copy filtered data (exclude *.template)
def pre_buildfs_action(source, target, env):
  process_template(source, target, env)
  prepare_filtered_data_dir(env)

try:
  env.AddPreAction("buildfs", pre_buildfs_action)
except Exception:
  pass
# Run after program loads to ensure files exist
env.AddPostAction("$PROGRAM_PATH", post_program_action)
