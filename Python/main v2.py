import os
import time
# --- CACHE FIX ---
os.environ['MPLCONFIGDIR'] = '/tmp'

from flask import Flask, request, make_response
from PIL import Image, ImageDraw, ImageFont, ImageOps
import requests
import sys
import io
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

app = Flask(__name__)

# --- CONFIG (GROQ & QWEN UPDATE) ---
GROQ_API_KEY = "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" 

FONT_PATH = "nikosh.ttf" # Ensure this file is in your folder
FONT_SIZE = 11      # ৩ লাইনের জন্য ফন্ট সাইজ ১১ করা হয়েছে
MAX_WIDTH = 128
LINE_SPACING = 10   # ৩ লাইনের জন্য লাইন স্পেসিং ১০ করা হয়েছে

# --- RAM CACHE ---
current_image_bytes = None
total_height = 32

# --- PIXEL PERFECT TEXT WRAPPING ---
def get_text_width(text, font):
    try:
        return font.getlength(text)
    except AttributeError:
        return font.getsize(text)[0]

def wrap_text_by_pixels(text, font, max_width):
    clean_text = " ".join(text.split())
    words = clean_text.split(' ')
    lines = []
    current_line = ""
    
    for word in words:
        if not word: continue
        test_line = f"{current_line} {word}".strip()
        
        if get_text_width(test_line, font) <= max_width:
            current_line = test_line
        else:
            if current_line:
                lines.append(current_line)
            current_line = word
            
    if current_line:
        lines.append(current_line)
    return lines

# --- MATH RENDERER ---
def render_latex_to_image(formula):
    try:
        if any(ord(c) > 127 for c in formula): return None 
        formula = formula.strip().replace('\n', ' ')
        
        fig = plt.figure(figsize=(3, 0.6), dpi=140)
        plt.rc('mathtext', fontset='cm')
        plt.rc('lines', linewidth=2.5) 
        
        fig.text(0.5, 0.5, f"${formula}$", fontsize=20, ha='center', va='center', weight='bold')
        
        buf = io.BytesIO()
        plt.axis('off')
        plt.savefig(buf, format='png', bbox_inches='tight', pad_inches=0.05)
        plt.close(fig)
        
        buf.seek(0)
        img = Image.open(buf).convert("L")
        img = ImageOps.invert(img) # White on Black
        
        if img.width > MAX_WIDTH:
            ratio = MAX_WIDTH / img.width
            new_h = int(img.height * ratio)
            img = img.resize((MAX_WIDTH, new_h), Image.Resampling.LANCZOS)
            
        img = img.point(lambda x: 255 if x > 80 else 0)
        return img.convert("1", dither=Image.Dither.NONE)
    except:
        return None

@app.route('/ask')
def ask_ai():
    global current_image_bytes, total_height
    
    raw_q = request.args.get('q', '').strip()
    print(f"\n=== INPUT: {raw_q} ===", file=sys.stderr)

    # --- PREFIX LOGIC ---
    mode_instruction = ""
    clean_q = raw_q
    
    if raw_q.lower().startswith("a.") or raw_q.lower().startswith("a "):
        mode_instruction =  ("MODE: SECTION A. Output: Exact definition in 1-2 sentences. No bullet points.")
        clean_q = raw_q[2:].strip()
    elif raw_q.lower().startswith("b.") or raw_q.lower().startswith("b "):
        mode_instruction =  ("MODE: SECTION B. Output: Theory and max 3 short points. Be direct.")
        clean_q = raw_q[2:].strip()
    elif raw_q.lower().startswith("c.") or raw_q.lower().startswith("c "):
        mode_instruction =  ("MODE: SECTION C. Output Structure: Intro -> Main Body -> Math Formula -> Conclusion.")
        clean_q = raw_q[2:].strip()
    else:
        mode_instruction = "MODE: General Answer. Keep it concise."

    # --- SYSTEM PROMPT (STRICT CONTINUOUS TEXT) ---
    system_instruction = (
        "You are an expert Economics Professor for National University, Bangladesh. "
        "CRITICAL INSTRUCTION: You are outputting text for a TINY 128px screen. "
        "1. DO NOT use line breaks (\\n). Write in continuous paragraphs. "
        "2. DO NOT use lists or bullet points unless explicitly asked. "
        "3. Answer ONLY in PURE BANGLA SCRIPT (No Banglish). "
        "4. Keep English terms in brackets. "
        "5. Math Formulas MUST be wrapped in $$ $$. "
        f"Structure: {mode_instruction}"
    )
    
    # --- GROQ API CALL WITH AUTO RETRY ---
    url = "https://api.groq.com/openai/v1/chat/completions"
    headers = {
        "Authorization": f"Bearer {GROQ_API_KEY}",
        "Content-Type": "application/json"
    }
    
    data = {
        "model": "qwen/qwen3.8-27b", # Qwen model for best Bangla output
        "messages": [
            {"role": "system", "content": system_instruction},
            {"role": "user", "content": f"Student Question: {clean_q}"}
        ],
        "temperature": 0.3,
        "max_tokens": 1500
    }
    
    answer = "Error: Init"
    
    # Auto-Retry Logic (3 Attempts for Rate Limits/Overload)
    for attempt in range(3):
        try:
            response = requests.post(url, headers=headers, json=data)
            if response.status_code == 200:
                ans_json = response.json()
                if 'choices' in ans_json and len(ans_json['choices']) > 0:
                    answer = ans_json['choices'][0]['message']['content'].replace("**", "")
                    print(f"AI ANS: {answer[:50]}...", file=sys.stderr)
                else:
                    answer = "API Error: No answer."
                break
                
            elif response.status_code in [429, 503]:
                wait_time = (attempt + 1) * 3
                print(f"Groq API Limit {response.status_code}. Retrying in {wait_time}s... {attempt+1}/3", file=sys.stderr)
                answer = "Server Overloaded. Retrying..."
                time.sleep(wait_time)
                
            else:
                answer = f"API Error: {response.status_code} - {response.text}"
                print(answer, file=sys.stderr)
                break
        except Exception as e:
            answer = f"Error: {str(e)}"
            break

    # --- RENDERER (Text + Math) ---
    try:
        font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    except:
        font = ImageFont.load_default()

    segments = answer.split('$$')
    rendered_elements = [] 
    
    for i, segment in enumerate(segments):
        segment = segment.strip()
        if not segment: continue
        
        if i % 2 == 1: # Math Segment
            math_img = render_latex_to_image(segment)
            if math_img:
                rendered_elements.append((math_img, math_img.height))
            else:
                lines = wrap_text_by_pixels(segment, font, MAX_WIDTH)
                chunk_h = len(lines) * LINE_SPACING
                if chunk_h > 0:
                    txt_img = Image.new('1', (MAX_WIDTH, chunk_h), 0)
                    d = ImageDraw.Draw(txt_img)
                    y = 0
                    for line in lines:
                        d.text((2, y), line, font=font, fill=1)
                        y += LINE_SPACING
                    rendered_elements.append((txt_img, chunk_h))
        else: # Text Segment (Bangla)
            lines = wrap_text_by_pixels(segment, font, MAX_WIDTH)
            chunk_h = len(lines) * LINE_SPACING 
            if chunk_h > 0:
                txt_img = Image.new('1', (MAX_WIDTH, chunk_h), 0)
                d = ImageDraw.Draw(txt_img)
                y = 0
                for line in lines:
                    d.text((0, y), line, font=font, fill=1)
                    y += LINE_SPACING
                rendered_elements.append((txt_img, chunk_h))

    total_h = sum(h for _, h in rendered_elements) + 10
    if total_h < 32: total_h = 32

    final_img = Image.new('1', (MAX_WIDTH, total_h), 0)
    current_y = 1   # একদম উপর থেকে লেখা শুরুর জন্য 1 করা হয়েছে
    d_final = ImageDraw.Draw(final_img)
    
    for i, (element, h) in enumerate(rendered_elements):
        if isinstance(element, str):
            d_final.text((0, current_y), element, font=font, fill=1)
        else:
            if i % 2 == 1: # Math (Center align)
                 x_pos = (MAX_WIDTH - element.width) // 2
            else:
                 x_pos = 0 # Text (Left align)
            final_img.paste(element, (x_pos, current_y))
        current_y += h + 5

    total_height = total_h
    ssd1306_bytes = bytearray()
    pixels = final_img.load()
    
    for y in range(total_h):
        for x in range(0, 128, 8):
            byte = 0
            for bit in range(8):
                if x + bit < 128:
                    if pixels[x + bit, y]:
                        byte |= (1 << bit)
            ssd1306_bytes.append(byte)
            
    current_image_bytes = bytes(ssd1306_bytes)
    return f"OK:{total_h}"

@app.route('/view')
def view_chunk():
    global current_image_bytes
    if current_image_bytes is None: return make_response(b'\x00' * 512)
    y = int(request.args.get('y', 0))
    
    start_row = y
    end_row = start_row + 32
    start_byte = start_row * 16
    end_byte = end_row * 16
    
    total_len = len(current_image_bytes)
    
    if start_byte >= total_len:
        chunk = b'\x00' * 512
    else:
        chunk = current_image_bytes[start_byte : end_byte]
        if len(chunk) < 512:
            chunk += b'\x00' * (512 - len(chunk))
            
    return make_response(chunk)

if __name__ == '__main__':
    port = int(os.environ.get('SERVER_PORT', 10976)) 
    app.run(host='0.0.0.0', port=port)
