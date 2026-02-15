#!/bin/bash

ENDPOINT="http://localhost:4566"
REGION="ap-southeast-2"

echo "=========================================="
echo "  IoT System Launcher"
echo "=========================================="

# ============================================
# STEP 1: Setup LocalStack Infrastructure
# ============================================
echo ""
echo "[1/4] Setting up LocalStack infrastructure..."

# Check if DynamoDB Table exists
if aws dynamodb describe-table --table-name DeviceTelemetry --endpoint-url $ENDPOINT --region $REGION >/dev/null 2>&1; then
    echo "✓ DynamoDB table 'DeviceTelemetry' already exists"
else
    echo "Creating DynamoDB table..."
    aws dynamodb create-table \
        --table-name DeviceTelemetry \
        --attribute-definitions \
            AttributeName=device_id,AttributeType=S \
            AttributeName=timestamp,AttributeType=S \
        --key-schema \
            AttributeName=device_id,KeyType=HASH \
            AttributeName=timestamp,KeyType=RANGE \
        --billing-mode PAY_PER_REQUEST \
        --endpoint-url $ENDPOINT \
        --region $REGION
    echo "✓ Table created"
fi

# Enable TTL
echo "Enabling TTL (auto-delete old records)..."
aws dynamodb update-time-to-live \
    --table-name DeviceTelemetry \
    --time-to-live-specification "Enabled=true, AttributeName=ttl" \
    --endpoint-url $ENDPOINT \
    --region $REGION 2>/dev/null || echo "✓ TTL already enabled"

# Package Lambda
echo "Packaging Lambda function..."
python3 -c "import zipfile; z = zipfile.ZipFile('lambda.zip', 'w'); z.write('lambda_function.py'); z.close()"

# Deploy Lambda
if aws lambda get-function --function-name ProcessTelemetry --endpoint-url $ENDPOINT --region $REGION >/dev/null 2>&1; then
    echo "Updating Lambda function..."
    aws lambda update-function-code \
        --function-name ProcessTelemetry \
        --zip-file fileb://lambda.zip \
        --endpoint-url $ENDPOINT \
        --region $REGION > /dev/null
    echo "✓ Lambda updated"
else
    echo "Creating Lambda function..."
    aws lambda create-function \
        --function-name ProcessTelemetry \
        --runtime python3.9 \
        --handler lambda_function.lambda_handler \
        --role arn:aws:iam::000000000000:role/lambda-role \
        --zip-file fileb://lambda.zip \
        --endpoint-url $ENDPOINT \
        --region $REGION > /dev/null
    echo "✓ Lambda created"
fi

# ============================================
# STEP 2: Start MQTT Bridge (Background)
# ============================================
echo ""
echo "[2/4] Starting MQTT → Lambda bridge..."
python mqtt_to_lambda.py > mqtt_bridge.log 2>&1 &
MQTT_PID=$!
echo "✓ MQTT bridge running (PID: $MQTT_PID)"
echo "  Logs: mqtt_bridge.log"

# Give it a moment to connect
sleep 10

# ============================================
# STEP 3: Start Streamlit Dashboard (Background)
# ============================================
echo ""
echo "[3/4] Starting Streamlit dashboard..."
python -m streamlit run dashboard.py --server.headless=true > streamlit.log 2>&1 &
STREAMLIT_PID=$!
echo "✓ Dashboard running (PID: $STREAMLIT_PID)"
echo "  URL: http://localhost:8501"
echo "  Logs: streamlit.log"

# ============================================
# STEP 4: Summary & Keep Running
# ============================================
echo ""
echo "=========================================="
echo "  🚀 All Systems Running!"
echo "=========================================="
echo ""
echo "Dashboard:     http://localhost:8501"
echo "MQTT Bridge:   Running (check mqtt_bridge.log)"
echo "LocalStack:    http://localhost:4566"
echo ""
echo "Press Ctrl+C to stop all services..."
echo ""

# Cleanup function
cleanup() {
    echo ""
    echo "Stopping services..."
    kill $MQTT_PID $STREAMLIT_PID 2>/dev/null
    echo "✓ All services stopped"
    exit 0
}

# Trap Ctrl+C
trap cleanup INT TERM

# Keep script running forever (until Ctrl+C)
while true; do
    # Check if processes are still running
    if ! kill -0 $MQTT_PID 2>/dev/null; then
        echo "⚠️  MQTT bridge crashed! Check mqtt_bridge.log"
        cleanup
    fi
    if ! kill -0 $STREAMLIT_PID 2>/dev/null; then
        echo "⚠️  Dashboard crashed! Check streamlit.log"
        cleanup
    fi
    sleep 5
done