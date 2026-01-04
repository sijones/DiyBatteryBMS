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
        'handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
      if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
      if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
      AckUpdate('can_rx_pin');
      AckUpdate('can_tx_pin');
      AckUpdate('can_en_pin');
      '''
    },
    'esp32s3-ESPCAN': {
        'title': 'ESPCAN Configuration',
        'fields': '''<div class="form-row">
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
        'handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
      if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
      if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
      AckUpdate('can_rx_pin');
      AckUpdate('can_tx_pin');
      AckUpdate('can_en_pin');
      '''
    },
    'esp32dev': {
        'title': 'CAN Bus Configuration',
        'fields': '''<div class="form-group">
            <label for="canbuscspin">CAN CS Pin:</label>
            <input type="number" id="canbuscspin" onchange="EnqueueUpdate('canbuscspin')" onkeypress="HandleEnter(event, 'canbuscspin')">
          </div>''',
        'handlers': '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
      AckUpdate('canbuscspin');
      '''
    },
    'esp32plus': {
        'title': 'CAN Bus Configuration',
        'fields': '''<div class="form-group">
            <label for="canbuscspin">CAN CS Pin:</label>
            <input type="number" id="canbuscspin" onchange="EnqueueUpdate('canbuscspin')" onkeypress="HandleEnter(event, 'canbuscspin')">
          </div>''',
        'handlers': '''if(obj.hasOwnProperty('canbuscspin')) document.getElementById('canbuscspin').value=obj.canbuscspin;
      AckUpdate('canbuscspin');
      '''
    },
    'esp32c3-ESPCAN': {
        'title': 'ESPCAN Configuration',
        'fields': '''<div class="form-row">
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
        'handlers': '''if(obj.hasOwnProperty('can_rx_pin')) document.getElementById('can_rx_pin').value=obj.can_rx_pin;
      if(obj.hasOwnProperty('can_tx_pin')) document.getElementById('can_tx_pin').value=obj.can_tx_pin;
      if(obj.hasOwnProperty('can_en_pin')) document.getElementById('can_en_pin').value=obj.can_en_pin;
      AckUpdate('can_rx_pin');
      AckUpdate('can_tx_pin');
      AckUpdate('can_en_pin');
      ''',
        'hide_fan': True  # C3 doesn't support MCPWM/FAN
    }
}

def process_template(source, target, env, output_dir=None):
  """Process the templates and generate the output HTML files."""
  try:
    env_name = str(env['PIOENV'])
    project_dir = env['PROJECT_DIR']
    # REQUIRED: output_dir must be specified (no default to data/ anymore)
    if output_dir is None:
      raise ValueError("output_dir must be explicitly specified for process_template()")
    os.makedirs(output_dir, exist_ok=True)

    # Get configuration for this environment (robust fallback)
    config = CAN_CONFIGS.get(env_name)
    if config is None and 'espcan' in env_name.lower():
      # Any ESPCAN variant should use the ESPCAN field set
      config = CAN_CONFIGS.get('esp32-ESPCAN', CAN_CONFIGS['esp32dev'])
    if config is None:
      config = CAN_CONFIGS['esp32dev']

    # FAN field configuration (hide on C3)
    fan_field = ''
    fan_handler = ''
    if not config.get('hide_fan', False):
      fan_field = '''<div class="form-group">
            <label for="fanpin">FAN Pin:</label>
            <input type="number" id="fanpin" onchange="EnqueueUpdate('fanpin')" onkeypress="HandleEnter(event, 'fanpin')">
          </div>'''
      fan_handler = '''if(obj.hasOwnProperty('fanpin')) document.getElementById('fanpin').value=obj.fanpin;
      AckUpdate('fanpin');'''

    # Process index.htm
    index_template = os.path.join(project_dir, 'data', 'index.htm.template')
    # Check for hidden template first
    if not os.path.exists(index_template):
      index_template_hidden = index_template + '.hidden'
      if os.path.exists(index_template_hidden):
        index_template = index_template_hidden
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
    # Check for hidden template first
    if not os.path.exists(ap_template):
      ap_template_hidden = ap_template + '.hidden'
      if os.path.exists(ap_template_hidden):
        ap_template = ap_template_hidden
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



def _restore_templates_if_needed(data_dir):
  """Ensure visible templates exist if a previous run left them hidden."""
  for fname in ['index.htm.template', 'index-ap.htm.template']:
    visible = os.path.join(data_dir, fname)
    hidden = visible + '.hidden'
    if os.path.exists(hidden) and not os.path.exists(visible):
      try:
        os.rename(hidden, visible)
        print("[HTML] Restored missing template before build: %s" % fname)
      except Exception as e:
        print("[HTML] Warning: Could not restore %s: %s" % (fname, str(e)))


def prepare_data_for_buildfs(source, target, env):
  """Generate HTML files into data/ directory and hide templates before filesystem build."""
  project_dir = env['PROJECT_DIR']
  data_dir = os.path.join(project_dir, 'data')
  
  _restore_templates_if_needed(data_dir)
  
  # Render templates directly into data/ FIRST (will create index.htm and index-ap.htm)
  process_template(None, None, env, output_dir=data_dir)
  
  # THEN hide template files by renaming them (buildfs won't see them)
  for fname in ['index.htm.template', 'index-ap.htm.template']:
    src = os.path.join(data_dir, fname)
    dst = os.path.join(data_dir, fname + '.hidden')
    if os.path.exists(src):
      try:
        if os.path.exists(dst):
          os.remove(dst)
        os.rename(src, dst)
        print("[HTML] Temporarily hidden: %s" % fname)
      except Exception as e:
        print("[HTML] Warning: Could not hide %s: %s" % (fname, str(e)))
  
  print("[HTML] Filesystem templates rendered into data/ directory")

def cleanup_data_after_buildfs(source, target, env):
  """Remove generated HTML files and restore templates after filesystem build."""
  project_dir = env['PROJECT_DIR']
  data_dir = os.path.join(project_dir, 'data')
  
  # Remove generated files
  for fname in ['index.htm', 'index-ap.htm']:
    fpath = os.path.join(data_dir, fname)
    if os.path.exists(fpath):
      try:
        os.remove(fpath)
        print("[HTML] Cleaned up generated file: %s" % fname)
      except Exception as e:
        print("[HTML] Warning: Could not remove %s: %s" % (fname, str(e)))
  
  # Restore template files
  for fname in ['index.htm.template', 'index-ap.htm.template']:
    src = os.path.join(data_dir, fname + '.hidden')
    dst = os.path.join(data_dir, fname)
    if os.path.exists(src):
      try:
        if os.path.exists(dst):
          os.remove(dst)
        os.rename(src, dst)
        print("[HTML] Restored: %s" % fname)
      except Exception as e:
        print("[HTML] Warning: Could not restore %s: %s" % (fname, str(e)))

# Wire pre/post actions around the buildfs target so templates are restored afterward
try:
  env.AddPreAction("buildfs", prepare_data_for_buildfs)
  env.AddPostAction("buildfs", cleanup_data_after_buildfs)
except Exception as e:
  print("[HTML] Error registering buildfs hooks: %s" % str(e))
  import traceback
  traceback.print_exc()
