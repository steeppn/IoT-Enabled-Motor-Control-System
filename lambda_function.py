import json
import boto3
import os
from datetime import datetime, timezone, timedelta
from decimal import Decimal
from boto3.dynamodb.conditions import Key

# Setup resources
endpoint_url = f"http://{os.environ.get('LOCALSTACK_HOSTNAME')}:4566" if os.environ.get('LOCALSTACK_HOSTNAME') else None
dynamodb = boto3.resource('dynamodb', endpoint_url=endpoint_url)
table = dynamodb.Table('DeviceTelemetry')

def lambda_handler(event, context):
    # Initial Parsing
    if isinstance(event, str):
        payload = json.loads(event, parse_float=Decimal)
    else:
        payload = json.loads(json.dumps(event), parse_float=Decimal)

    # THE CLEANING/FLATTENING STEP
    # If the ESP32 wrapped the data in a "body" string, we unpack it here
    if 'body' in payload and isinstance(payload['body'], str):
        try:
            nested_data = json.loads(payload['body'], parse_float=Decimal)
            payload.update(nested_data) # Move temp, speed, etc. to top level
            del payload['body']         # Remove the messy string
        except Exception as e:
            print(f"Could not parse nested body: {e}")

    # Ensure basic identity exists
    device_id = payload.get('device_id', 'esp32-001')
    new_temp = payload.get('temp')

    # FILTERING (Only save if change is > 0.5)
    response = table.query(
        KeyConditionExpression=Key('device_id').eq(device_id),
        ScanIndexForward=False, 
        Limit=1
    )
    items = response.get('Items', [])
    
    if items and new_temp is not None:
        last_temp = items[0].get('temp')
        if abs(Decimal(str(new_temp)) - Decimal(str(last_temp))) < Decimal('0.5'):
            print(f"Filtered: Change too small ({new_temp} vs {last_temp})")
            return {'statusCode': 200, 'body': 'Filtered'}
        
    # TTL
    ttl = int((datetime.now(timezone.utc) + timedelta(days=3)).timestamp())
    payload['ttl'] = ttl

    # FINAL PREP & STORE
    payload['device_id'] = device_id
    payload['timestamp'] = datetime.now(timezone.utc).isoformat()
    
    try:
        table.put_item(Item=payload)
        print(f"Data Flattened & Stored: {payload}")
    except Exception as e:
        print(f"DynamoDB Error: {str(e)}")
        return {'statusCode': 500, 'body': str(e)}
    
    return {'statusCode': 200, 'body': 'Stored successfully'}