import gzip
import os
import re
from datetime import datetime

INPUT_HTML = 'index.html'      
OUTPUT_HEADER = 'WebPage.h'    

def minify_html(html_str):
    # HTML কমেন্ট মুছে ফেলা
    html_str = re.sub(r'', '', html_str, flags=re.DOTALL)
    # ট্যাগের মাঝখানের অপ্রয়োজনীয় স্পেস মুছে ফেলা
    html_str = re.sub(r'>\s+<', '><', html_str)
    # একাধিক স্পেস বা নিউলাইনকে একটি মাত্র স্পেসে রূপান্তর করা
    html_str = re.sub(r'\s{2,}', ' ', html_str)
    return html_str.strip()

def compress_and_convert():
    if not os.path.exists(INPUT_HTML):
        print(f"Error: '{INPUT_HTML}' file not found!")
        return

    # UTF-8 এনকোডিংয়ে HTML ফাইলটি পড়া (ইমোজির জন্য জরুরি)
    with open(INPUT_HTML, 'r', encoding='utf-8') as f:
        original_html = f.read()

    original_size = len(original_html.encode('utf-8'))

    # Minify করা
    minified_html = minify_html(original_html)
    minified_bytes = minified_html.encode('utf-8')
    minified_size = len(minified_bytes)

    # Gzip কম্প্রেশন করা
    compressed_data = gzip.compress(minified_bytes)
    compressed_size = len(compressed_data)
    
    # বাইনারি ডেটাকে Hexadecimal C Array তে কনভার্ট করা এবং সাজানো (প্রতি লাইনে ১৬টি বাইট)
    hex_list = [f"0x{b:02X}" for b in compressed_data]
    formatted_hex = ""
    for i in range(0, len(hex_list), 16):
        line = ", ".join(hex_list[i:i+16])
        formatted_hex += f"    {line},\n"
    
    # শেষের কমা এবং নিউলাইন মুছে ফেলা
    formatted_hex = formatted_hex.rstrip(",\n")
    
    # বর্তমান সময় (Timestamp)
    current_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")

    # হেডার ফাইলের স্ট্রাকচার তৈরি করা
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

    # হেডার ফাইলটি সেভ করা
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