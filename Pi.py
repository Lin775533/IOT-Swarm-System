import socket
import time
import threading
import RPi.GPIO as GPIO
from datetime import datetime

# GPIO Setup
GPIO.setmode(GPIO.BCM)
GPIO.setwarnings(False)

# LED Pins
RED_LED = 27
GREEN_LED = 23
YELLOW_LED = 22
WHITE_LED = 24

# Button Pin
RESET_BUTTON = 15

# Setup pins
for pin in [RED_LED, GREEN_LED, YELLOW_LED, WHITE_LED]:
    GPIO.setup(pin, GPIO.OUT)
    GPIO.output(pin, GPIO.LOW)

GPIO.setup(RESET_BUTTON, GPIO.IN, pull_up_down=GPIO.PUD_DOWN)

# Network settings
UDP_PORT = 2910
BROADCAST_IP = '192.168.1.255'  # Broadcast address for your network

# Device tracking
device_data = {}
device_led_assignments = {}  # Permanent mapping of device IDs to LEDs
available_leds = [RED_LED, GREEN_LED, YELLOW_LED]  # LEDs available for assignment

class LightSwarm:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(('', UDP_PORT))
        
        self.current_master = None
        self.running = True
        self.system_active = True
        
        # Start threads
        threading.Thread(target=self.receive_data, daemon=True).start()
        threading.Thread(target=self.handle_button, daemon=True).start()
        threading.Thread(target=self.update_leds, daemon=True).start()
        
        print("LightSwarm initialized and ready")
        print(f"Listening on port {UDP_PORT}")
        print("Available LEDs:", available_leds)

    def receive_data(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(1024)
                message = data.decode('utf-8')
                if message.startswith('MASTER:'):  # Only print MASTER messages
                    print(f"Received: {message} from {addr}")
                self.handle_message(message, addr)
            except Exception as e:
                print(f"Error receiving data: {e}")

    def handle_message(self, message, addr):
        if not self.system_active:
            return
            
        # Only process MASTER messages
        if message.startswith('MASTER:'):
            try:
                _, device_id, reading = message.split(':')
                device_id = int(device_id)
                reading = int(reading)
                
                # Assign LED if this is a new device
                if device_id not in device_led_assignments and available_leds:
                    device_led_assignments[device_id] = available_leds.pop(0)
                    print(f"Assigned LED {device_led_assignments[device_id]} to Device {device_id}")
                
                # Update device data
                device_data[device_id] = {
                    'reading': reading,
                    'last_seen': time.time(),
                    'addr': addr[0]
                }
                
                self.current_master = device_id
                self.log_data(device_id, reading)
                
                print(f"Master Device {device_id}: Reading = {reading}")
            except Exception as e:
                print(f"Error handling master message: {e}")

    def send_reset(self):
        print("\nRESET initiated...")
        # Turn off all LEDs except WHITE
        for led in [RED_LED, GREEN_LED, YELLOW_LED]:
            GPIO.output(led, GPIO.LOW)
        
        # Clear device tracking data
        device_data.clear()
        device_led_assignments.clear()
        available_leds.clear()
        available_leds.extend([RED_LED, GREEN_LED, YELLOW_LED])
        self.current_master = None
        self.system_active = False
        
        # Send reset command
        print("Sending RESET command to all ESPs")
        self.sock.sendto(b'RESET', (BROADCAST_IP, UDP_PORT))
        
        # Turn on WHITE LED
        print("System is in reset state. Press button again to activate")
        GPIO.output(WHITE_LED, GPIO.HIGH)
        time.sleep(3)  # Wait for 3 seconds
        GPIO.output(WHITE_LED, GPIO.LOW)

    def send_activate(self):
        print("Sending ACTIVATE command to all ESPs")
        self.system_active = True
        self.sock.sendto(b'ACTIVATE', (BROADCAST_IP, UDP_PORT))
        GPIO.output(WHITE_LED, GPIO.LOW)
        print("System reactivated")

    def handle_button(self):
        last_press_time = 0
        debounce_time = 0.5  # 500ms debounce
        
        while self.running:
            current_state = GPIO.input(RESET_BUTTON)
            current_time = time.time()
            
            if current_state == GPIO.HIGH and (current_time - last_press_time) > debounce_time:
                last_press_time = current_time
                
                if self.system_active:
                    print("\nReset button pressed - Resetting system")
                    self.send_reset()
                else:
                    print("\nReset button pressed - Activating system")
                    self.send_activate()
            
            time.sleep(0.1)

    def update_leds(self):
        while self.running:
            if not self.system_active:
                time.sleep(0.1)
                continue
                
            try:
                # Turn off all LEDs first
                for led in [RED_LED, GREEN_LED, YELLOW_LED]:
                    GPIO.output(led, GPIO.LOW)
                
                current_time = time.time()
                
                # Clean up old devices
                for device_id in list(device_data.keys()):
                    if current_time - device_data[device_id]['last_seen'] > 5:
                        print(f"Device {device_id} timed out")
                        if device_id in device_led_assignments:
                            print(f"Returning LED {device_led_assignments[device_id]} to available pool")
                            available_leds.append(device_led_assignments[device_id])
                            GPIO.output(device_led_assignments[device_id], GPIO.LOW)
                            del device_led_assignments[device_id]
                        del device_data[device_id]
                
                # Only flash LED for current master
                if self.current_master and self.current_master in device_led_assignments:
                    led_pin = device_led_assignments[self.current_master]
                    reading = device_data[self.current_master]['reading']
                    
                    flash_delay = self.calculate_flash_delay(reading)
                    GPIO.output(led_pin, GPIO.HIGH)
                    time.sleep(flash_delay)
                    GPIO.output(led_pin, GPIO.LOW)
                    time.sleep(flash_delay)
                
                time.sleep(0.1)
            except Exception as e:
                print(f"Error in update_leds: {e}")

    def calculate_flash_delay(self, reading):
        # Map reading (0-1023) to delay (0.1-1.0 seconds)
        return max(0.1, 1.0 - (reading / 1023.0 * 0.9))

    def log_data(self, device_id, reading):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        try:
            with open('lightswarm.log', 'a') as f:
                f.write(f"{timestamp},{device_id},{reading}\n")
        except Exception as e:
            print(f"Error logging data: {e}")

    def cleanup(self):
        print("\nCleaning up...")
        self.running = False
        for led in [RED_LED, GREEN_LED, YELLOW_LED, WHITE_LED]:
            GPIO.output(led, GPIO.LOW)
        GPIO.cleanup()
        self.sock.close()
        print("Cleanup complete")

def main():
    try:
        swarm = LightSwarm()
        print("\nLightSwarm started. Press Ctrl+C to exit.")
        print("Reset button on GPIO 15")
        print("Press button once to reset, again to activate")
        print("Waiting for ESP devices...")
        
        while True:
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\nShutdown requested...")
        swarm.cleanup()
    except Exception as e:
        print(f"\nUnexpected error: {e}")
        swarm.cleanup()

if __name__ == "__main__":
    main()
