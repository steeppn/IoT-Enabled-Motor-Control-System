import json
import boto3
import os
from decimal import Decimal
from datetime import datetime, timezone
from boto3.dynamodb.conditions import Key

# Use the internal hostname for LocalStack
endpoint_url = f"http://{os.environ.get('LOCALSTACK_HOSTNAME')}:4566" if os.environ.get('LOCALSTACK_HOSTNAME') else None
dynamodb = boto3.resource('dynamodb', endpoint_url=endpoint_url)
table = dynamodb.Table('DeviceTelemetry')
    
def lambda_handler(event, context):
    """Process IoT telemetry and store in DynamoDB"""
    
    # PARSE & PREPARE
    if isinstance(event, str):
        payload = json.loads(event, parse_float=Decimal)
    else:
        payload = json.loads(json.dumps(event), parse_float=Decimal)
    
    device_id = payload.get('device_id', 'esp32-001')
    new_temp = payload.get('temp')
    
    # THE FILTER CHECK (New Block)
    # Look up the last record for this specific device
    response = table.query(
        KeyConditionExpression=Key('device_id').eq(device_id),
        ScanIndexForward=False, # Newest first
        Limit=1
    )
    items = response.get('Items', [])

    if items and new_temp is not None:
        last_temp = items[0].get('temp')
        # Only save if the change is 0.5 or more
        if abs(new_temp - last_temp) < Decimal('0.5'):
            print(f"❄️ Filtered: Change too small ({new_temp} vs {last_temp})")
            return {'statusCode': 200, 'body': 'Filtered'}
    

    # FINISH PREPARATION
    payload['device_id'] = device_id
    payload['timestamp'] = datetime.now(timezone.utc).isoformat()
    
    # STORE
    try:
        table.put_item(Item=payload)
        print(f"✅ Significant change! Stored: {payload}")
    except Exception as e:
        print(f"❌ DynamoDB Error: {str(e)}")
        return {'statusCode': 500, 'body': str(e)}
    
    return {
        'statusCode': 200,
        'body': json.dumps('Telemetry stored successfully')
    }