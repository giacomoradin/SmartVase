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
# Note: Vision topics removed as ESP32-CAM now connects directly to Firebase
TOPICS_TO_SUBSCRIBE = [
    "smartvase/+/telemetry",
    "smartvase/+/logs",
    "smartvase/+/command/ack",
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

# --- Setup MQTT Client (Initialized early for global access) ---
client_id = f"firestore-bridge-{int(time.time())}"
# Compatibility wrapper for paho-mqtt v1.x and v2.x
try:
    # paho-mqtt v2.x requires callback_api_version
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=client_id)
except AttributeError:
    # fallback for paho-mqtt v1.x
    client = mqtt.Client(client_id=client_id)

# --- MQTT Client Callbacks (MQTT -> Firestore) ---

def on_connect(client, userdata, flags, rc):
    """Callback function when the client connects to the MQTT broker."""
    if rc == 0:
        print(f"Connected successfully to MQTT Broker: {HIVEMQ_HOST}")
        for topic in TOPICS_TO_SUBSCRIBE:
            client.subscribe(topic)
            print(f"Subscribed to: {topic}")
    else:
        print(f"Failed to connect, return code {rc}")

def on_disconnect(client, userdata, rc):
    """Callback function when the client disconnects."""
    print(f"Disconnected from MQTT Broker with code {rc}. Attempting reconnection...")

def on_message(client, userdata, msg):
    """Callback function when a message is received from ESP32."""
    topic = msg.topic
    payload_str = msg.payload.decode('utf-8')
    print(f"\nReceived message on topic: {topic}")

    topic_parts = topic.split('/')
    if len(topic_parts) < 3:
        print(f"WARN: Invalid topic structure ignored: {topic}")
        return

    # smartvase/{vase_id}/{message_type}
    vase_id = topic_parts[1]
    message_type = topic_parts[2]

    try:
        payload_json = json.loads(payload_str)
        if isinstance(payload_json, dict):
            if 'vase_id' not in payload_json and 'device_id' not in payload_json:
                payload_json['vase_id'] = vase_id
            payload_json['cloud_received_ts'] = firestore.SERVER_TIMESTAMP
        else:
            print(f"WARN: JSON payload is not a dictionary: {payload_str}")
            return
    except json.JSONDecodeError as e:
        print(f"ERROR: Failed to parse JSON payload on topic {topic}: {e}")
        return 

    try:
        if message_type == "telemetry":
            doc_ref = db.collection("smartvase").document(vase_id).collection("telemetry").document("telemetry")
            doc_ref.set(payload_json, merge=True)
            print(f"Firestore: Updated telemetry for {vase_id}")

        elif message_type == "logs":
            col_ref = db.collection("smartvase").document(vase_id).collection("logs")
            col_ref.add(payload_json)
            print(f"Firestore: Added log for {vase_id}")

        elif message_type == "command" and len(topic_parts) == 4 and topic_parts[3] == "ack":
            doc_ref = db.collection("smartvase").document(vase_id).collection("command").document("ack")
            doc_ref.set(payload_json, merge=False)
            print(f"Firestore: Updated command ACK for {vase_id}")

        else:
            print(f"WARN: Unhandled topic structure: {topic}")

    except Exception as e:
        print(f"ERROR: Failed to write to Firestore for topic {topic}: {e}")

# --- Firestore Listener Callback (Firestore -> MQTT) ---

def on_command_snapshot(col_snapshot, changes, read_time):
    """Callback triggered whenever a command document changes in Firestore."""
    for change in changes:
        # We only care if the Flutter app added or modified the document
        if change.type.name in ['ADDED', 'MODIFIED']:
            doc = change.document
            path_parts = doc.reference.path.split('/')
            
            # Expected path: smartvase/{vase_id}/command/{doc_id}
            if len(path_parts) != 4:
                print(f"WARN: Unexpected document path structure: {doc.reference.path}. Ignoring.")
                continue
                
            print(f"\nFirestore trigger: Document {doc.id} changed in {doc.reference.path}")

            vase_id = path_parts[1]
            payload_dict = doc.to_dict()
            
            # Convert Firestore dict back to JSON string for MQTT
            payload_json = json.dumps(payload_dict)
            mqtt_topic = f"smartvase/{vase_id}/command/{doc.id}"
            
            # Ensure we are only reacting to documents actually named 'config'
            if doc.id == 'config':
                print(f"\nFirestore Trigger: Config updated for {vase_id}. Publishing to MQTT...")
                # thread-safe publish, using QoS 1 and Retain=True for config data
                client.publish(mqtt_topic, payload_json, qos=1, retain=True)
            else:
                print(f"Got command: {doc.id}")
                client.publish(mqtt_topic, payload_json)

# Attach the Firestore background listener
# Using a collection_group query to listen to ALL subcollections named 'command'
print("Attaching Firestore listener for commands...")
config_watch = db.collection_group("command").on_snapshot(on_command_snapshot)

# --- Finalize MQTT Configuration and Run ---

# Assign callbacks
client.on_connect = on_connect
client.on_message = on_message
client.on_disconnect = on_disconnect

# Set username and password
client.username_pw_set(HIVEMQ_USER, HIVEMQ_PASS)

# Configure TLS for secure connection
client.tls_set(
    ca_certs=None,
    certfile=None,
    keyfile=None,
    cert_reqs=ssl.CERT_REQUIRED,
    tls_version=ssl.PROTOCOL_TLS_CLIENT,
    ciphers=None
)
client.tls_insecure_set(False)

# Connect and Start Loop
try:
    print(f"Attempting to connect to MQTT broker: {HIVEMQ_HOST}:{HIVEMQ_PORT}")
    client.connect(HIVEMQ_HOST, HIVEMQ_PORT, 60)
    # loop_forever handles reconnections and blocks the main thread
    # The Firestore on_snapshot listener runs safely on its own background thread
    client.loop_forever()
except Exception as e:
    print(f"CRITICAL ERROR: Could not connect/run MQTT client: {e}")
except KeyboardInterrupt:
    print("\nDisconnecting from MQTT broker...")
    client.disconnect()
    # Unsubscribe the Firestore watcher to close threads cleanly
    config_watch.unsubscribe()
    print("Script terminated.")