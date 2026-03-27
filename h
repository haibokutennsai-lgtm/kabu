# ---------------- 完全自動ポートフォリオBOT ----------------
import discord
from discord.ext import commands, tasks
import yfinance as yf
import matplotlib.pyplot as plt
import pandas as pd
import datetime, os, asyncio, smtplib
from fpdf import FPDF
from email.mime.multipart import MIMEMultipart
from email.mime.base import MIMEBase
from email import encoders
from email.mime.text import MIMEText

# ---------------- Discord設定 ----------------
intents = discord.Intents.default()
intents.message_content = True
bot = commands.Bot(command_prefix="!", intents=intents)

favorites = {}  # ユーザーごとの登録銘柄
alerts = {}     # アラート設定
emails = {}     # ユーザーのメールアドレス

# ---------------- メール設定 ----------------
SMTP_SERVER = "smtp.gmail.com"
SMTP_PORT = 587
EMAIL_ADDRESS = "あなたのメール@gmail.com"
EMAIL_PASSWORD = "アプリパスワード"

# ---------------- BOT起動 ----------------
@bot.event
async def on_ready():
    print("BOT起動成功")
    check_alerts.start()
    monitor_portfolio.start()
    daily_portfolio_pdf.start()
    weekly_portfolio_pdf.start()
    monthly_portfolio_pdf.start()
    save_excel_csv.start()
    daily_portfolio_email.start()
    weekly_portfolio_email.start()
    monthly_portfolio_email.start()

# ---------------- 株価取得 ----------------
def get_price(code, period="2y"):
    if code.lower() in ['btc','eth','doge','xrp']:
        code_yf = code.upper()+'-USD'
    elif code.isdigit() and not code.endswith(".T"):
        code_yf = code+".T"
    else:
        code_yf = code
    stock = yf.Ticker(code_yf)
    data = stock.history(period=period)
    price = data["Close"].iloc[-1]
    prev = data["Close"].iloc[-2] if len(data)>1 else price
    change = price-prev
    pct_change = (change/prev*100) if prev!=0 else 0
    volatility = data["Close"].pct_change().std()*100
    return price, change, pct_change, volatility, data, code_yf

# ---------------- 登録・一覧 ----------------
@bot.command()
async def 登録(ctx, code):
    user = ctx.author.id
    favorites.setdefault(user, []).append(code)
    await ctx.send(f"{code} を登録しました")

@bot.command()
async def 一覧(ctx):
    user = ctx.author.id
    await ctx.send(favorites.get(user, []))

# ---------------- アラート ----------------
@bot.command()
async def 通知(ctx, code, price: float, direction="above"):
    alerts.setdefault(code,[]).append((price,direction,ctx.channel.id))
    await ctx.send(f"{code} 通知設定しました: {price} {direction}")

@tasks.loop(seconds=60)
async def check_alerts():
    for code, alert_list in alerts.items():
        try:
            price, change, pct_change, vol, data, _ = get_price(code)
            for alert in alert_list[:]:
                target,direction,channel_id = alert
                trigger = (direction=="above" and price>=target) or (direction=="below" and price<=target)
                if trigger:
                    channel = bot.get_channel(channel_id)
                    await channel.send(f"{code} 条件到達！現在値: {price}")
                    alert_list.remove(alert)
        except:
            continue

# ---------------- ポートフォリオ監視 ----------------
@tasks.loop(minutes=5)
async def monitor_portfolio():
    for user, codes in favorites.items():
        for code in codes:
            try:
                price, change, pct, vol, data, _ = get_price(code)
                if abs(pct)>=5:
                    user_obj = await bot.fetch_user(user)
                    await user_obj.send(f"{code} 5%以上変動！ 現在値:{price} ({pct:.2f}%)")
            except:
                continue

# ---------------- チャート作成 ----------------
def create_full_chart(data, code, days=60):
    data = data[-days:]
    data['MA5'] = data['Close'].rolling(5).mean()
    data['MA20'] = data['Close'].rolling(20).mean()
    fig, ax = plt.subplots(figsize=(10,6))
    ax.plot(data.index,data['Close'],label='Close',color='blue')
    ax.plot(data.index,data['MA5'],label='MA5',color='orange')
    ax.plot(data.index,data['MA20'],label='MA20',color='green')
    ax.set_title(f"{code} 株価チャート")
    ax.legend()
    timestamp=datetime.datetime.now().strftime("%Y%m%d%H%M%S")
    filename=f"fullchart_{code}_{timestamp}.png"
    plt.tight_layout()
    plt.savefig(filename)
    plt.close()
    return filename

@bot.command()
async def チャート(ctx, code):
    try:
        _,_,_,_,data,_ = get_price(code)
        filename=create_full_chart(data, code)
        await ctx.send(file=discord.File(filename))
        os.remove(filename)
    except:
        await ctx.send("取得できませんでした")

# ---------------- PDF作成 ----------------
def create_pdf_chart(user_id, codes, days=60):
    pdf = FPDF()
    pdf.set_auto_page_break(auto=True, margin=15)
    for code in codes:
        try:
            _,_,_,_,data,_=get_price(code,period=f"{days}d")
            filename=create_full_chart(data,code,days)
            pdf.add_page()
            pdf.set_font("Arial","B",16)
            pdf.cell(0,10,f"{code} チャート",0,1)
            pdf.image(filename,x=10,y=30,w=190)
            os.remove(filename)
        except:
            continue
    timestamp=datetime.datetime.now().strftime("%Y%m%d%H%M%S")
    pdf_filename=f"portfolio_{user_id}_{timestamp}.pdf"
    pdf.output(pdf_filename)
    return pdf_filename

# ---------------- Excel/CSV保存 ----------------
@tasks.loop(hours=24)
async def save_excel_csv():
    for user, codes in favorites.items():
        data_list=[]
        for code in codes:
            try:
                price, change, pct, vol, data, _ = get_price(code)
                data_list.append({'コード':code,'価格':price,'変化':change,'変化率(%)':pct,'ボラティリティ(%)':vol})
            except:
                continue
        if data_list:
            df=pd.DataFrame(data_list)
            timestamp=datetime.datetime.now().strftime("%Y%m%d%H%M%S")
            df.to_excel(f"portfolio_{user}_{timestamp}.xlsx",index=False)
            df.to_csv(f"portfolio_{user}_{timestamp}.csv",index=False)

# ---------------- メール登録・送信 ----------------
@bot.command()
async def メール登録(ctx, email):
    emails[ctx.author.id] = email
    await ctx.send(f"{email} をメール送信先として登録しました")

def send_email(to_email, subject, body, attachments=[]):
    msg = MIMEMultipart()
    msg['From']=EMAIL_ADDRESS
    msg['To']=to_email
    msg['Subject']=subject
    msg.attach(MIMEText(body,'plain'))
    for file in attachments:
        part=MIMEBase('application','octet-stream')
        with open(file,'rb') as f:
            part.set_payload(f.read())
        encoders.encode_base64(part)
        part.add_header('Content-Disposition',f'attachment; filename="{os.path.basename(file)}"')
        msg.attach(part)
    with smtplib.SMTP(SMTP_SERVER,SMTP_PORT) as server:
        server.starttls()
        server.login(EMAIL_ADDRESS,EMAIL_PASSWORD)
        server.send_message(msg)

async def send_portfolio_email(user, codes):
    if user not in emails: return
    attachments=[]
    try: attachments.append(create_pdf_chart(user, codes))
    except: pass
    if attachments:
        send_email(emails[user],"ポートフォリオレポート","自動生成レポートです",attachments)
        for f in attachments: os.remove(f)

@tasks.loop(hours=24)
async def daily_portfolio_email():
    for user,codes in favorites.items():
        await send_portfolio_email(user,codes)

# ---------------- BOT起動 ----------------
bot.run("YOUR_BOT_TOKEN")
