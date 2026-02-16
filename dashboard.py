import streamlit as st
import boto3
import pandas as pd
import plotly.express as px
from streamlit_autorefresh import st_autorefresh # <--- New Import
from decimal import Decimal
import logging

# Mute MSYS2 thread warning
logging.getLogger("streamlit.runtime.scriptrunner_utils.script_run_context").setLevel(logging.ERROR)

# --- CONFIG ---
LOCALSTACK_ENDPOINT = "http://localhost:4566"
TABLE_NAME = "DeviceTelemetry"

st.set_page_config(page_title="IoT Sweep Dashboard", layout="wide")

# --- AUTO REFRESH ---
# Pings the server every 
st_autorefresh(interval=2000, key="datarefresh")

st.title("ERAS-ESP32 Real-Time Telemetry")

def fetch_data():
    try:
        dynamodb = boto3.resource("dynamodb", endpoint_url=LOCALSTACK_ENDPOINT, region_name="ap-southeast-2", aws_access_key_id='test', aws_secret_access_key='test')
        table = dynamodb.Table(TABLE_NAME)
        response = table.scan()
        data = response.get('Items', [])
        return pd.DataFrame(data) if data else pd.DataFrame()
    except Exception as e:
        st.error(f"Connection Error: {e}")
        return pd.DataFrame()

# --- MAIN RENDER ---
df = fetch_data()

if not df.empty and 'temp' in df.columns:
    # Data Cleaning 
    df['temp'] = df['temp'].apply(float)
    if 'current' in df.columns:
        df['current'] = df['current'].apply(float)

    df['timestamp'] = pd.to_datetime(df['timestamp'])
    df = df.sort_values('timestamp')

    # Metrics Row
    col1, col2, col3 = st.columns(3)
    latest = df.iloc[-1]
    if latest['temp'] >= 29:
        col1.metric("Current Temp", f"{latest['temp']}°C", delta="Warning", delta_color="yellow", delta_arrow="off")
    else:
        col1.metric("Current Temp", f"{latest['temp']}°C")
    
    if (latest.get('current') >= 1.2):
        col2.metric("Current Draw", f"{latest.get('current', 0)}A", delta="Warning", delta_color="yellow", delta_arrow="off")
    else:
        col2.metric("Current Draw", f"{latest.get('current', 0)}A")

    status = latest.get('status', 'Unknown')
    faulted = int(latest.get('faulted', 0))
    if faulted >= 1:
        col3.metric("Status", "FAULT", delta="Critical", delta_color="red", delta_arrow="off")
    elif status == 'RUNNING':
        col3.metric("Status", "RUNNING")
    else:
        col3.metric("Status", "STOPPED")
                    

    # Chart
    fig = px.line(df, x='timestamp', y='temp', title="Temperature History")
    st.plotly_chart(fig, width='stretch')

    fig2 = px.line(df, x='timestamp', y='current', title="Current Draw History")
    st.plotly_chart(fig2, width='stretch')
    
    st.write("### Raw Data", df.tail(5))
else:
    st.info("Dashboard connected. Waiting for ESP32 data...")