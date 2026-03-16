import gzip
import os
import re
from datetime import datetime

INPUT_HTML = 'index.html'      
OUTPUT_HEADER = 'WebPage.h'    

def minify_html(html_str):
    # Remove HTML Comments
    html_str = re.sub(r'', '', html_str, flags=re.DOTALL)
    # remove unnecessery spaching
    html_str = re.sub(r'>\s+<', '><', html_str)
    # make a single spech from multi newline
    html_str = re.sub(r'\s{2,}', ' ', html_str)
    return html_str.strip()

def compress_and_convert():
    if not os.path.exists(INPUT_HTML):
        print(f"Error: '{INPUT_HTML}' file not found!")
        return

    # UTF-8 Enchoding for emoji and other UNICODE supports
    with open(INPUT_HTML, 'r', encoding='utf-8') as f:
        original_html = f.read()

    original_size = len(original_html.encode('utf-8'))

    # Minify
    minified_html = minify_html(original_html)
    minified_bytes = minified_html.encode('utf-8')
    minified_size = len(minified_bytes)

    # Gzip compression
    compressed_data = gzip.compress(minified_bytes)
    compressed_size = len(compressed_data)
    
    # Convert binary data to Hexadecimal C Array(16 byte each line)
    hex_list = [f"0x{b:02X}" for b in compressed_data]
    formatted_hex = ""
    for i in range(0, len(hex_list), 16):
        line = ", ".join(hex_list[i:i+16])
        formatted_hex += f"    {line},\n"
    
    # Remove comma's and unnecessery new line
    formatted_hex = formatted_hex.rstrip(",\n")
    
    # (Timestamp)
    current_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")

    # Header file structure for CypherNode_PRO projectrs
    header_content = f"""/**
 * @file {OUTPUT_HEADER}
 * @brief Auto-generated Minified & Gzipped HTML file
 * @date {current_time}
 */

#ifndef WEBPAGE_H
#define WEBPAGE_H

#include <Arduino.h>

// Gzipped HTML payload ({compressed_size} bytes)
const uint8_t index_html_gz[] PROGMEM = {{
{formatted_hex}
}};

// Length of the gzipped payload
const size_t index_html_gz_len = {compressed_size};

#endif // WEBPAGE_H
"""

    # Save header file on project location
    with open(OUTPUT_HEADER, 'w', encoding='utf-8') as f:
        f.write(header_content)

    print("-" * 40)
    print("Conversion Successful!")
    print(f"Original Size : {original_size} bytes")
    print(f"Minified Size : {minified_size} bytes")
    print(f"Gzipped Size  : {compressed_size} bytes")
    print("-" * 40)
    print(f"Generated '{OUTPUT_HEADER}' with timestamp: {current_time}")

if __name__ == '__main__':
    compress_and_convert()