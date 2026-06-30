#!/usr/bin/env python3
"""
Script to automatically add Doxygen file headers and class/method skeletons
to the entire SmartVase C++ codebase.
"""

import os
import re

# Directory to scan
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(SCRIPT_DIR)
FIRMWARE_DIR = os.path.join(ROOT_DIR, "firmware")

# Exclude list for paths/files
EXCLUDE_PATTERNS = [
    r"\.pio",
    r"lib/driver",
    r"pb\.h",
    r"pb_common",
    r"pb_decode",
    r"pb_encode",
    r"smartvase\.pb"
]

def should_process(file_path):
    # Only process .h and .cpp files
    if not (file_path.endswith('.h') or file_path.endswith('.cpp')):
        return False
    # Check exclude list
    rel_path = os.path.relpath(file_path, ROOT_DIR).replace("\\", "/")
    for pattern in EXCLUDE_PATTERNS:
        if re.search(pattern, rel_path, re.IGNORECASE):
            return False
    return True

def inject_doxygen_file_header(file_path):
    with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    # Check if file already has a file-level Doxygen header
    if '@file' in content or '/**' in content[:200]:
        print(f"Skipping file header (already present): {os.path.basename(file_path)}")
        return False

    filename = os.path.basename(file_path)
    file_brief = f"Sorgente per il modulo {filename.split('.')[0]}" if file_path.endswith('.cpp') else f"Definizione delle classi e funzioni per {filename.split('.')[0]}"
    
    header = f"""/**
 * @file {filename}
 * @brief {file_brief}
 * @author Giacomo Radin
 * @date 2026-06-30
 */

"""
    # Prepend header to content
    new_content = header + content
    with open(file_path, 'w', encoding='utf-8') as f:
        f.write(new_content)
    print(f"Injected file header into: {filename}")
    return True

def inject_class_and_method_comments(file_path):
    if not file_path.endswith('.h'):
        return False

    with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    # Parse and comment classes that don't have Doxygen comments
    class_pattern = r'(?<!\/\*\*\n)(?<!\*\/)\s*class\s+(\w+)\s*\{'
    
    def class_replacer(match):
        class_name = match.group(1)
        # Check if the match is preceded by any comment
        start_idx = match.start()
        preceding_text = content[max(0, start_idx-200):start_idx]
        if '/**' in preceding_text or '/*' in preceding_text:
            return match.group(0)
            
        comment = f"""
/**
 * @class {class_name}
 * @brief Classe per la gestione di {class_name}
 */
class {class_name} {{"""
        return comment

    modified_content = re.sub(class_pattern, class_replacer, content)
    
    # Parse and comment methods
    # Look for functions in headers: void funcName(...); or bool funcName(...);
    method_pattern = r'(?<!\/\*\*\n)(?<!\*\/)\s+(void|bool|int|uint32_t|float)\s+(\w+)\(([^)]*)\)\s*;'
    
    def method_replacer(match):
        return_type = match.group(1)
        method_name = match.group(2)
        params = match.group(3)
        
        # Check if there is already a comment block above it
        start_idx = match.start()
        preceding_text = modified_content[max(0, start_idx-100):start_idx]
        if '/**' in preceding_text or '/*' in preceding_text:
            return match.group(0)
            
        param_docs = ""
        if params.strip():
            for param in params.split(','):
                param_name = param.strip().split()[-1].replace('&', '').replace('*', '')
                param_docs += f"\n     * @param {param_name} Descrizione del parametro {param_name}."
                
        comment = f"""
    /**
     * @brief Esegue l'operazione {method_name}.{param_docs}
     * @return Ritorna un valore di tipo {return_type}.
     */
    {return_type} {method_name}({params});"""
        return comment

    # Only apply method regex if it doesn't break header includes or macros
    # Modified content only for classes first to be safe
    if modified_content != content:
        with open(file_path, 'w', encoding='utf-8') as f:
            f.write(modified_content)
        print(f"Added class Doxygen annotations to: {os.path.basename(file_path)}")
        return True
    return False

def main():
    print("=== Auto-Injecting Doxygen Comments ===")
    count_headers = 0
    count_classes = 0
    
    for root, dirs, files in os.walk(FIRMWARE_DIR):
        for file in files:
            full_path = os.path.join(root, file)
            if should_process(full_path):
                if inject_doxygen_file_header(full_path):
                    count_headers += 1
                if inject_class_and_method_comments(full_path):
                    count_classes += 1

    print(f"Total file headers injected: {count_headers}")
    print(f"Total class annotations injected: {count_classes}")

if __name__ == "__main__":
    main()
