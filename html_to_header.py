import gzip
import os
import re
from datetime import datetime

INPUT_HTML = 'index.html'      
OUTPUT_HEADER = 'WebPage.h'    

def minify_css(css):
    css = re.sub(r'/\*.*?\*/', '', css, flags=re.DOTALL)
    css = re.sub(r'\s*([{:;,])\s*', r'\1', css)
    css = re.sub(r'\s+', ' ', css)
    # Shorten colors: #ffffff -> #fff
    css = re.sub(r'#([0-9a-fA-F])\1([0-9a-fA-F])\2([0-9a-fA-F])\3(?=[;}\s])', r'#\1\2\3', css)
    # Remove 0 units: 0px -> 0
    css = re.sub(r'(^|[:\s])0(?:px|em|pt|%)', r'\g<1>0', css)
    css = re.sub(r';}', '}', css)
    return css.strip()

def minify_js(js):
    js = re.sub(r'/\*.*?\*/', '', js, flags=re.DOTALL)
    js = re.sub(r'(?<![:"\'/])//.*', '', js)
    # Aggressive whitespace removal
    js = re.sub(r'\s*([{}()=+\-*/,:;<>?!&|^%])\s*', r'\1', js)
    # Shorten booleans
    js = js.replace('true', '!0').replace('false', '!1')
    js = re.sub(r'\s+', ' ', js)
    return js.strip()

def minify_html(html_str):
    # CSS & JS minification
    html_str = re.sub(r'<style>(.*?)</style>', lambda m: f"<style>{minify_css(m.group(1))}</style>", html_str, flags=re.DOTALL)
    html_str = re.sub(r'<script>(.*?)</script>', lambda m: f"<script>{minify_js(m.group(1))}</script>", html_str, flags=re.DOTALL)
    
    # Remove Comments
    html_str = re.sub(r'<!--.*?-->', '', html_str, flags=re.DOTALL)
    # Remove unnecessary attribute quotes (only if no special chars)
    html_str = re.sub(r'([a-z-]+)="([a-z0-9-]+)"', r'\1=\2', html_str, flags=re.IGNORECASE)
    # Remove self-closing slashes
    html_str = re.sub(r'\s*/>', '>', html_str)
    # Whitespace cleanup
    html_str = re.sub(r'>\s+<', '><', html_str)
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
    
    # Calculate sizes in KB
    original_kb = original_size / 1024
    minified_kb = minified_size / 1024
    compressed_kb = compressed_size / 1024
    
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

// Original: {original_kb:.2f} KB | Minified: {minified_kb:.2f} KB | Gzipped: {compressed_kb:.2f} KB
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
    print(f"Original Size : {original_kb:.2f} KB")
    print(f"Minified Size : {minified_kb:.2f} KB")
    print(f"Gzipped Size  : {compressed_kb:.2f} KB")
    print("-" * 40)
    print(f"Generated '{OUTPUT_HEADER}' with timestamp: {current_time}")

if __name__ == '__main__':
    compress_and_convert()