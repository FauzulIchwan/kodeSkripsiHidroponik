import cv2
import os
import torch
import torch.nn.functional as F
import torchvision.models as models
import torchvision.transforms as transforms
from datetime import datetime
import requests
from picamera2 import Picamera2, Preview
from libcamera import Transform
from PIL import Image
from io import BytesIO
import numpy as np
import paho.mqtt.client as mqtt
import json
import time
import socket
import base64

MODEL_PATH = '/home/pi/resnet50_bayamv4.pth'

# Recreate the model architecture
model = models.resnet50(weights=False)
num_classes = 3
model.fc = torch.nn.Linear(model.fc.in_features, num_classes)
model.load_state_dict(torch.load(MODEL_PATH, map_location=torch.device('cpu')))
model.eval()

# Konfigurasi MQTT
BROKER_ADDRESS = "103.195.142.180"  # Ganti dengan IP/server broker MQTT
BROKER_PORT = 1883  # Port default MQTT
MQTT_TOPIC = "dataRaspi"
MQTT_TOPIC_IMAGE ="image"

# Image preprocessing pipeline
transform = transforms.Compose([
    transforms.Resize((224, 224)),
    transforms.ToTensor(),
    transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225])
])

def check_internet(host="8.8.8.8", port=53, timeout=3):
    try:
        socket.setdefaulttimeout(timeout)
        socket.socket(socket.AF_INET, socket.SOCK_STREAM).connect((host, port))
        return True
    except socket.error:
        return False

def capture_image():
    camera = Picamera2(1)
    try:
        # Configure the camera for still image capture
        capture_config = camera.create_still_configuration(transform=Transform(vflip=True))
        camera.resolution = (320, 240)
        camera.configure(capture_config)

        print("Starting camera...")
        camera.start()

        # Capture image
        frame = camera.capture_array()  # Capture image as numpy array
        print("Image captured successfully.")
        return frame
    except Exception as e:
        print(f"Error capturing image: {e}")
        return None
    finally:
        camera.stop()
        camera.close()
        print("Camera stopped and closed.")

def send_mqtt(data):
    try:
        client = mqtt.Client()
        client.connect(BROKER_ADDRESS, BROKER_PORT, 60)

        # Publish data to MQTT
        client.publish(MQTT_TOPIC, json.dumps(data))
        print(f"Data sent to MQTT")
        client.disconnect()
    except Exception as e:
        print(f"Error sending MQTT: {e}")

def predict_and_send(image):
    # Convert numpy array to tensor
    processed_image = transform(image).unsqueeze(0)

    with torch.no_grad():
        outputs = model(processed_image)
        probabilities = F.softmax(outputs, dim=1)
        _, predicted_class = torch.max(outputs, 1)
        predicted_class = predicted_class.item()
    probabilities_list = probabilities.squeeze().tolist()
    print(f"Predicted class: {predicted_class}")
    print(f"Class probabilities: {probabilities_list}")

    # Konversi gambar ke format JPEG
    buffer = BytesIO()
    image.save(buffer, format="JPEG", quality=50)  # Compress image
    buffer.seek(0)
    image_data = buffer.getvalue()
    encoded_image = base64.b64encode(buffer.getvalue()).decode('utf-8')
    print(f"Buffer size: {len(buffer.getvalue())} bytes")
    # Prepare data for sending
    data = {
        'prediction': predicted_class,
        'timestamp': datetime.now().isoformat(),
        'confidence' : probabilities_list
    }

    while True:
        if check_internet():
            print("Internet connected. Sending data...")
            send_mqtt(data)
            send_gambar(image_data)
            break
        else:
            print("No internet connection. Retrying in 60 seconds...")
            time.sleep(60)

def send_gambar(image) :
    try:
        data = {
            'gambar' : image.hex(),
        };

        # URL endpoint Node-RED
        url = "http://103.195.142.180:1880/upload"
        auth = ('noderedIot', 'asdqwe')  # Sesuaikan dengan username dan password
        response = requests.post(url, json=data, auth=auth, timeout=60)

        print(f"Status Code: {response.status_code}")
        print(f"Response: {response.text}")

        if response.status_code == 200:
            print("Image sent successfully!")
        elif response.status_code == 401:
            print("Authentication failed! Check credentials.")
        else:
            print(f"Unexpected error: {response.status_code}")
    except Exception as e:
        print(f"Error sending HTTP: {e}")

if __name__ == '__main__':
    while True:
        frame = capture_image()
        if frame is not None:
            image = Image.fromarray(frame)  # Convert numpy array to PIL Image
            predict_and_send(image)
        else:
            print("Failed to capture image. Retrying in 12 hours.")

        # Tunggu 12 jam (12 * 60 * 60 detik)
        time.sleep(12 * 60 * 60)
