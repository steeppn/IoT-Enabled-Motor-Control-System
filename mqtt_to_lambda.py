import paho.mqtt.client as mqtt
import boto3
import json

# LocalStack Lambda client
lambda_client = boto3.client(
    'lambda',
    endpoint_url='http://localhost:4566',
    region_name='ap-southeast-2',
    aws_access_key_id='test',
    aws_secret_access_key='test'
)

def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT broker with code {rc}")
    client.subscribe("eras_esp32/telemetry")
    print("Subscribed to eras_esp32/telemetry")

def on_message(client, userdata, msg):
    print(f"Received: {msg.topic} -> {msg.payload.decode()}")
    
    # Invoke Lambda function with the payload
    try:
        response = lambda_client.invoke(
            FunctionName='ProcessTelemetry',
            InvocationType='Event',  # Async
            Payload=json.dumps({'body': msg.payload.decode()})
        )
        print(f"Lambda invoked: {response['StatusCode']}")
    except Exception as e:
        print(f"Error invoking Lambda: {e}")

# Connect to PUBLIC Mosquitto broker
client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect("test.mosquitto.org", 1883, 60)

print("MQTT to Lambda bridge started...")
client.loop_forever()