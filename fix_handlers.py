#!/usr/bin/env python3
import re

# Read templates
for template_file in ['data/index.htm.template']:
    with open(template_file, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Replace all onblur="SendJSONUpdate('fieldId')" with onchange + Enter handler
    pattern = r'onblur="SendJSONUpdate\(\'([^\']+)\'\)"'
    replacement = r"onchange=\"EnqueueUpdate('\1')\" onkeypress=\"HandleEnter(event, '\1')\""
    content = re.sub(pattern, replacement, content)
    
    # Write back
    with open(template_file, 'w', encoding='utf-8') as f:
        f.write(content)
    
    print(f"Replaced all onblur handlers in {template_file}")
