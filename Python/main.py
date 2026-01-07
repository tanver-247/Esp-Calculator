import os
# --- CACHE FIX ---
os.environ['MPLCONFIGDIR'] = '/tmp'

from flask import Flask, request, make_response
from PIL import Image, ImageDraw, ImageFont, ImageOps
import textwrap
import requests
import json
import sys
import io
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

app = Flask(__name__)

# --- CONFIG ---
GROQ_API_KEY = "gskyourgrokapi" 
GROQ_MODEL = "llama-3.3-70b-versatile"

FONT_PATH = "nikosh.ttf" # Ensure this file is in your folder
FONT_SIZE = 14
MAX_WIDTH = 128

# --- RAM CACHE ---
current_image_bytes = None
total_height = 32

# --- MATH RENDERER (High Contrast) ---
def render_latex_to_image(formula):
    try:
        # Filter out complex characters if needed, but Math is usually pure ASCII
        if any(ord(c) > 127 for c in formula): return None 
        formula = formula.strip().replace('\n', ' ')
        
        # High DPI for clear text on OLED
        fig = plt.figure(figsize=(3, 0.6), dpi=140)
        plt.rc('mathtext', fontset='cm')
        plt.rc('lines', linewidth=2.5) 
        
        # Render Math
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
            
        # Hard Threshold for clean bitmap
        img = img.point(lambda x: 255 if x > 80 else 0)
        return img.convert("1", dither=Image.Dither.NONE)
    except:
        return None

@app.route('/ask')
def ask_groq():
    global current_image_bytes, total_height
    
    raw_q = request.args.get('q', '').strip()
    print(f"\n=== INPUT: {raw_q} ===", file=sys.stderr)

    # --- PREFIX LOGIC (A/B/C) ---
    mode_instruction = ""
    clean_q = raw_q
    
    # Detect Mode (a/b/c)
    if raw_q.lower().startswith("a.") or raw_q.lower().startswith("a "):
        mode_instruction =  ( "MODE: SECTION A (Brief Answer). "
            "Output: Exact definition in 1-2 sentences only. No bullet points. 1 Example.")
        clean_q = raw_q[2:].strip()
    elif raw_q.lower().startswith("b.") or raw_q.lower().startswith("b "):
        mode_instruction =   ("MODE: SECTION B (Short Question). "
            "Output: Theory about the topic and 4-5 Bullet points. Be direct. Mention key Economists names. maximum outputs to write about 2 page on A4 paper on exam")
        clean_q = raw_q[2:].strip()
    elif raw_q.lower().startswith("c.") or raw_q.lower().startswith("c "):
        mode_instruction = ( "MODE: SECTION C (Broad Essay). Output: Intro -> Assumptions -> Math/Graph Description -> Conclusion."
         "MODE: SECTION C (Broad Essay). "
            "Output Structure: "
            "1. Introduction (Definiton). "
            "2. Assumptions (if any theory). "
            "3. Mathematical Analysis (Use $$ Formula $$)./Math/Graph Description "
            "4. Main Body (Points). "
            "5. Conclusion. "
            "Length: Detailed (800+ words)." )
        clean_q = raw_q[2:].strip()
    else:
        mode_instruction = "MODE: General Math/Theory Answer. Keep it concise."

    # --- SYSTEM PROMPT (THE FORCE BANGLA UPDATE) ---
    system_instruction = (
        "You are an expert Economics Professor for National University (NU), Bangladesh. "
        "CRITICAL INSTRUCTION: The user is typing on a T9 keyboard in English/Banglish (e.g., 'chahida kake bole'). "
        "YOU MUST TRANSLATE THE INTENT AND ANSWER IN PURE BANGLA SCRIPT. "
        "DO NOT OUTPUT BANGLISH. DO NOT OUTPUT ENGLISH (Except for specific technical terms). "
        "Structure: "
        f"{mode_instruction} "
        "Style Guidelines: "
        "1. Main Language: Bangla (বাংলা). "
        "2. Key Terms: Keep English terms in brackets (e.g., 'উপযোগ (Utility)'). "
        "3. Math Formulas: WRAP IN $$ $$  use ONLY English variables (e.g., $$ Q_d = a - bP $$). "
        "4. Show step-by-step calculation for Math problems. "
         "5. If a Graph is needed, draw it textually (e.g. 0I___x) -  (e.g., 'Draw a X/Y axis, slope goes down...'). "
        "6. Tone: Academic, Formal, Exam-Oriented. "
        "7. If the input is math (e.g., '10+10'), solve it and explain in Bangla."
    )
    
    # GROQ API CALL
    url = "https://api.groq.com/openai/v1/chat/completions"
    headers = {"Authorization": f"Bearer {GROQ_API_KEY}", "Content-Type": "application/json"}
    data = {
        "model": GROQ_MODEL,
        "messages": [
            {"role": "system", "content": system_instruction},
            {"role": "user", "content": f"Student Question: {clean_q}"}
        ],
        "temperature": 0.3, 
        "max_tokens": 1500
    }
    
    answer = "Error: Init"
    try:
        response = requests.post(url, headers=headers, json=data)
        if response.status_code == 200:
            ans_json = response.json()
            if 'choices' in ans_json:
                answer = ans_json['choices'][0]['message']['content'].replace("**", "")
                print(f"AI ANS: {answer[:50]}...", file=sys.stderr)
        else:
            answer = f"API Error: {response.status_code}"
    except Exception as e:
        answer = f"Error: {str(e)}"

    # --- RENDERER (Text + Math) ---
    try:
        font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    except:
        font = ImageFont.load_default()

    segments = answer.split('$$')
    rendered_elements = [] 
    wrapper = textwrap.TextWrapper(width=20) # Slightly wider wrapper for Bangla
    
    for i, segment in enumerate(segments):
        segment = segment.strip()
        if not segment: continue
        
        if i % 2 == 1: # Math Segment
            math_img = render_latex_to_image(segment)
            if math_img:
                rendered_elements.append((math_img, math_img.height))
            else: 
                # Fallback
                lines = wrapper.wrap(segment)
                chunk_h = len(lines) * 18
                if chunk_h > 0:
                    txt_img = Image.new('1', (MAX_WIDTH, chunk_h), 0)
                    d = ImageDraw.Draw(txt_img)
                    y = 0
                    for line in lines:
                        d.text((2, y), line, font=font, fill=1)
                        y += 18
                    rendered_elements.append((txt_img, chunk_h))
        else: # Text Segment (Bangla)
            # TextWrapper might struggle with Bangla width, but PIL handles rendering
            # We split by newlines manually to respect paragraphs
            paragraphs = segment.split('\n')
            for p in paragraphs:
                lines = wrapper.wrap(p)
                chunk_h = len(lines) * 20 # More spacing for Bangla chars
                if chunk_h > 0:
                    txt_img = Image.new('1', (MAX_WIDTH, chunk_h), 0)
                    d = ImageDraw.Draw(txt_img)
                    y = 0
                    for line in lines:
                        d.text((0, y), line, font=font, fill=1)
                        y += 20
                    rendered_elements.append((txt_img, chunk_h))

    total_h = sum(h for _, h in rendered_elements) + 10
    if total_h < 32: total_h = 32
    
    print(f"Img Height: {total_h}", file=sys.stderr)

    final_img = Image.new('1', (MAX_WIDTH, total_h), 0)
    current_y = 5
    d_final = ImageDraw.Draw(final_img)
    
    for element, h in rendered_elements:
        if isinstance(element, str):
            # Should not happen logic moved above, but safety check
            d_final.text((0, current_y), element, font=font, fill=1)
        else:
            # Center Math, Left Align Text
            if i % 2 == 1: # Math
                 x_pos = (MAX_WIDTH - element.width) // 2
            else:
                 x_pos = 0
            final_img.paste(element, (x_pos, current_y))
        current_y += h + 5

    total_height = total_h
    ssd1306_bytes = bytearray()
    pixels = final_img.load()
    
    # Binary conversion for SSD1306
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
    
    # Calculate byte offset
    # Each row is 128 pixels wide = 16 bytes.
    # SSD1306 Vertical Addressing is complex, but for simple horizontal mapping:
    # We are sending a linear buffer that the ESP32 draws with drawXBM
    # 128 width * 32 height = 4096 pixels = 512 bytes per chunk.
    
    # Map scrollY (pixels) to Byte Offset
    # The ESP32 requests 'y'. We assume y is the pixel row.
    # But XBM expects full data. 
    # Let's verify how the ESP32 code uses it. 
    # The ESP32 code requests 'y' but expects 512 bytes to fill a 128x32 screen.
    
    start_row = y
    end_row = start_row + 32
    
    # Extract the slice of rows
    # Our 'current_image_bytes' is row-major (1 byte = 8 pixels horizontal).
    # 16 bytes per row.
    
    start_byte = start_row * 16
    end_byte = end_row * 16
    
    total_len = len(current_image_bytes)
    
    if start_byte >= total_len:
        chunk = b'\x00' * 512
    else:
        chunk = current_image_bytes[start_byte : end_byte]
        # Padding if end of image reached
        if len(chunk) < 512:
            chunk += b'\x00' * (512 - len(chunk))
            
    return make_response(chunk)

if __name__ == '__main__':
    port = int(os.environ.get('SERVER_PORT', 5000))
    app.run(host='0.0.0.0', port=port)
