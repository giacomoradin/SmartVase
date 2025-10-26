import os
import paho.mqtt.client as mqtt
import json
import ssl
import time
import firebase_admin
from firebase_admin import credentials, firestore

print("Starting MQTT Listener...")

# --- Configuration (Read from Environment Variables) ---
try:
    HIVEMQ_HOST = os.environ['HIVEMQ_HOST'] # e.g., 'YOUR_UNIQUE_URL.s1.eu.hivemq.cloud'
    HIVEMQ_PORT = int(os.environ.get('HIVEMQ_PORT', 8883)) # Default to 8883 for TLS
    HIVEMQ_USER = os.environ['HIVEMQ_USER']
    HIVEMQ_PASS = os.environ['HIVEMQ_PASS']
    FIREBASE_CRED_PATH = os.environ['GOOGLE_APPLICATION_CREDENTIALS'] # Path to service account key file
except KeyError as e:
    print(f"ERROR: Missing environment variable: {e}")
    print("Please set HIVEMQ_HOST, HIVEMQ_USER, HIVEMQ_PASS, and GOOGLE_APPLICATION_CREDENTIALS")
    exit(1)

# Topics to subscribe to (using MQTT wildcard '+')
# Adjust these based on smartvase_data_structure.md
TOPICS_TO_SUBSCRIBE = [
    "smartvase/+/telemetry",
    "smartvase/+/logs",
    "smartvase/+/vision/image",
    # Other topics the ESP32-HUB might publish go here (e.g., responses if any)
]

# --- Initialize Firebase Admin ---
try:
    cred = credentials.Certificate(FIREBASE_CRED_PATH)
    firebase_admin.initialize_app(cred)
    db = firestore.client()
    print("Firebase Admin SDK initialized successfully.")
except Exception as e:
    print(f"ERROR: Failed to initialize Firebase Admin SDK: {e}")
    exit(1)

# --- MQTT Client Callbacks ---

def on_connect(client, userdata, flags, rc):
    """Callback function when the client connects to the MQTT broker."""
    if rc == 0:
        print(f"Connected successfully to MQTT Broker: {HIVEMQ_HOST}")
        # Subscribe to topics upon successful connection
        for topic in TOPICS_TO_SUBSCRIBE:
            client.subscribe(topic)
            print(f"Subscribed to: {topic}")
    else:
        print(f"Failed to connect, return code {rc}")

def on_disconnect(client, userdata, rc):
    """Callback function when the client disconnects."""
    print(f"Disconnected from MQTT Broker with code {rc}. Attempting reconnection...")

def on_message(client, userdata, msg):
    """Callback function when a message is received."""
    topic = msg.topic
    payload_str = msg.payload.decode('utf-8')
    print(f"\nReceived message on topic: {topic}")
    # print(f"Payload: {payload_str}") # Uncomment for debugging

    # --- Parse Topic ---
    topic_parts = topic.split('/')
    if len(topic_parts) < 3:
        print(f"WARN: Invalid topic structure ignored: {topic}")
        return

    # smartvase/{device_id}/{message_type}/{optional_subtype}
    # Example: smartvase/HUB_01/telemetry
    # Example: smartvase/HUB_01/vision/image
    device_id = topic_parts[1]
    message_type = topic_parts[2]
    sub_type = topic_parts[3] if len(topic_parts) > 3 else None

    # --- Parse Payload ---
    try:
        payload_json = json.loads(payload_str)
        # Add device_id to json payload if not already present
        if 'device_id' not in payload_json:
            payload_json['device_id'] = device_id
        # Add a server-side timestamp for when the message was processed
        payload_json['cloud_received_ts'] = firestore.SERVER_TIMESTAMP
    except json.JSONDecodeError as e:
        print(f"ERROR: Failed to parse JSON payload on topic {topic}: {e}")
        return # Skip message processing if payload is invalid

    # --- Write to Firestore based on Topic ---
    try:
        if message_type == "telemetry":
            # Path: smartvase/{device_id}/telemetry/telemetry
            doc_ref = db.collection("smartvase").document(device_id).collection("telemetry").document("telemetry")
            doc_ref.set(payload_json, merge=True) # Use set with merge=True to update/create
            print(f"Firestore: Updated telemetry for {device_id}")

        elif message_type == "logs":
            # Path: smartvase/{device_id}/logs/{auto_id}
            col_ref = db.collection("smartvase").document(device_id).collection("logs")
            col_ref.add(payload_json) # add() creates a new doc with auto-ID
            print(f"Firestore: Added log for {device_id}")

        elif message_type == "vision" and sub_type == "image":
            # Path: smartvase/{device_id}/vision/image (collection)
            # Use a specific document ID like 'latest' or 'image' within the collection
            doc_ref = db.collection("smartvase").document(device_id).collection("vision").document("image")
            doc_ref.set(payload_json, merge=True)
            print(f"Firestore: Updated image notification for {device_id}")

        # --- Add 'else if' blocks for other expected message types ---
        # Example: Handling command responses from ESP32
        # elif message_type == "response":
        #    # Decide where to store responses, maybe update original command doc?
        #    # This requires knowing the original command ID from the payload.
        #    cmd_id = payload_json.get('cmd_id')
        #    if cmd_id:
        #        # Assuming commands are stored in smartvase/{device_id}/commands/commands/{cmd_id}
        #        doc_ref = db.collection("smartvase").document(device_id).collection("commands").document(str(cmd_id))
        #        doc_ref.update({ # Use update to add response fields
        #             'response_status': payload_json.get('status'),
        #             'response_detail': payload_json.get('detail'),
        #             'response_exec_time_ms': payload_json.get('exec_time_ms'),
        #             'response_received_ts': firestore.SERVER_TIMESTAMP
        #        })
        #        print(f"Firestore: Updated command response for {cmd_id} on {device_id}")
        #    else:
        #        print(f"WARN: Received response without cmd_id on topic {topic}")


        else:
            print(f"WARN: Unhandled topic structure: {topic}")

    except Exception as e:
        print(f"ERROR: Failed to write to Firestore for topic {topic}: {e}")


# --- Setup MQTT Client ---
client_id = f"firestore-bridge-{int(time.time())}" # Create a unique client ID
client = mqtt.Client(client_id=client_id)

# Assign callbacks
client.on_connect = on_connect
client.on_message = on_message
client.on_disconnect = on_disconnect

# Set username and password
client.username_pw_set(HIVEMQ_USER, HIVEMQ_PASS)

# Configure TLS for secure connection
client.tls_set(
    ca_certs=None, # System CAs should work for Let's Encrypt/ISRG Root X1
    certfile=None,
    keyfile=None,
    cert_reqs=ssl.CERT_REQUIRED,
    tls_version=ssl.PROTOCOL_TLS_CLIENT, # Use appropriate TLS version
    ciphers=None
)
client.tls_insecure_set(False) # Ensure certificate is validated

# --- Connect and Start Loop ---
try:
    print(f"Attempting to connect to MQTT broker: {HIVEMQ_HOST}:{HIVEMQ_PORT}")
    client.connect(HIVEMQ_HOST, HIVEMQ_PORT, 60) # 60-second keepalive
    # loop_forever() is a blocking call that handles reconnections automatically
    client.loop_forever()
except Exception as e:
    print(f"CRITICAL ERROR: Could not connect/run MQTT client: {e}")
    # Consider adding system-level alerts here if the script fails to start
except KeyboardInterrupt:
    print("\nDisconnecting from MQTT broker...")
    client.disconnect()
    print("Script terminated.")
