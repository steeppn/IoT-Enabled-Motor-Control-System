#!/bin/bash

ENDPOINT="http://localhost:4566"
REGION="ap-southeast-2"

echo "Setting up LocalStack..."

# 1. Check if DynamoDB Table exists
if aws dynamodb describe-table --table-name DeviceTelemetry --endpoint-url $ENDPOINT --region $REGION >/dev/null 2>&1; then
    echo "⚠️  DynamoDB table 'DeviceTelemetry' already exists. Skipping..."
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
fi

# 2. Package Lambda (using Python to avoid "zip command not found")
echo "Packaging Lambda function..."
python3 -c "import zipfile; z = zipfile.ZipFile('lambda.zip', 'w'); z.write('lambda_function.py'); z.close()"

# 3. Check if Lambda Function exists
if aws lambda get-function --function-name ProcessTelemetry --endpoint-url $ENDPOINT --region $REGION >/dev/null 2>&1; then
    echo "⚠️  Lambda 'ProcessTelemetry' already exists. Updating code..."
    aws lambda update-function-code \
        --function-name ProcessTelemetry \
        --zip-file fileb://lambda.zip \
        --endpoint-url $ENDPOINT \
        --region $REGION
else
    echo "Creating Lambda function..."
    aws lambda create-function \
        --function-name ProcessTelemetry \
        --runtime python3.9 \
        --handler lambda_function.lambda_handler \
        --role arn:aws:iam::000000000000:role/lambda-role \
        --zip-file fileb://lambda.zip \
        --endpoint-url $ENDPOINT \
        --region $REGION
fi

echo "✅ LocalStack setup complete!"