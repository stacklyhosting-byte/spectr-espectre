"""
Micro-ESPectre - MQTT Handler Module

Handles MQTT communication and command processing.
Manages connection, publishing state updates, and processing remote commands.

Author: Francesco Pace <francesco.pace@gmail.com>
License: GPLv3
"""
import json
import time
from umqtt.simple import MQTTClient
from src.mqtt.commands import MQTTCommands


class MQTTHandler:
    """MQTT handler with publishing and command support"""
    
    def __init__(self, config, detector, wlan, traffic_generator=None, band_calibration_func=None, global_state=None, ml_detector=None):
        """
        Initialize MQTT handler
        
        Args:
            config: Configuration module
            detector: IDetector instance (MVSDetector — decision-maker)
            wlan: WLAN instance
            traffic_generator: TrafficGenerator instance (optional)
            band_calibration_func: Function to run band calibration (optional)
            global_state: GlobalState instance for accessing loop metrics (optional)
            ml_detector: MLDetector instance for logging-only ML scores (optional)
        """
        self.config = config
        self.detector = detector
        self.ml_detector = ml_detector
        self.wlan = wlan
        self.traffic_gen = traffic_generator
        self.band_calibration_func = band_calibration_func
        self.global_state = global_state
        self.client = None
        self.cmd_handler = None
        
        # Topics
        self.base_topic = config.MQTT_TOPIC
        self.cmd_topic = f"{config.MQTT_TOPIC}/cmd"
        self.response_topic = f"{config.MQTT_TOPIC}/response"
        
        # Publishing state
        self.last_variance = 0.0
        self.last_state = 0  # STATE_IDLE
        
    def connect(self):
        """Connect to MQTT broker"""
        self.client = MQTTClient(
            self.config.MQTT_CLIENT_ID,
            self.config.MQTT_BROKER,
            port=self.config.MQTT_PORT,
            user=self.config.MQTT_USERNAME,
            password=self.config.MQTT_PASSWORD
        )
        
        print('Connecting to MQTT broker...')
        self.client.connect()
        print('MQTT connected')
        
        # Initialize command handler (pass ML detector so factory_reset resets both)
        self.cmd_handler = MQTTCommands(
            self.client,
            self.config,
            self.detector,
            self.response_topic,
            self.wlan,
            self.traffic_gen,
            self.band_calibration_func,
            self.global_state,
            self.ml_detector
        )
        
        # Set callback for incoming messages
        self.client.set_callback(self._on_message)
        
        # Subscribe to command topic
        self.client.subscribe(self.cmd_topic)
        #print(f'Subscribed to: {self.cmd_topic}')
        
        return self.client
    
    def _on_message(self, topic, msg):
        """Callback for incoming MQTT messages"""
        try:
            topic_str = topic.decode('utf-8') if isinstance(topic, bytes) else topic
            
            if topic_str == self.cmd_topic:
                # Process command
                self.cmd_handler.process_command(msg)
            
        except Exception as e:
            print(f"Error processing MQTT message: {e}")
    
    def check_messages(self):
        """Check for incoming MQTT messages (non-blocking)"""
        try:
            self.client.check_msg()
        except Exception as e:
            print(f"Error checking MQTT messages: {e}")
    
    def publish_state(self, current_variance, current_state, current_threshold, 
                     packet_delta, dropped_delta, pps, turbulence=0, ml_score=0):
        """
        Publish current state to MQTT
        
        Args:
            current_variance: Current moving variance (MVS)
            current_state: Current state (0=IDLE, 1=MOTION)
            current_threshold: Current threshold
            packet_delta: Packets processed since last publish
            dropped_delta: Packets dropped since last publish
            pps: Packets per second
            turbulence: Current spatial turbulence value
            ml_score: ML detector score (0.0-10.0 scale, 0 = not running)
        """
        state_str = 'motion' if current_state == 1 else 'idle'
        
        payload = {
            'movement': round(current_variance, 4),
            'threshold': round(current_threshold, 4),
            'state': state_str,
            'packets_processed': packet_delta,
            'packets_dropped': dropped_delta,
            'pps': pps,
            'turbulence': round(turbulence, 4),
            'timestamp': time.time()
        }
        
        # Add ML score when ML detector is active
        if ml_score > 0:
            payload['ml_score'] = round(ml_score, 4)
        
        # Add gain_lock status from global state
        if self.global_state and hasattr(self.global_state, 'needs_cv_normalization'):
            payload['gain_lock'] = not self.global_state.needs_cv_normalization
        
        # Add algorithm name from config
        if hasattr(self.config, 'DETECTION_ALGORITHM'):
            payload['algorithm'] = self.config.DETECTION_ALGORITHM
        
        # Get RSSI from WiFi interface if available
        try:
            rssi = self.wlan.status('rssi')
            if rssi is not None:
                payload['rssi'] = rssi
        except:
            pass
        
        try:
            self.client.publish(self.base_topic, json.dumps(payload))
        except Exception as e:
            print(f"Error publishing to MQTT: {e}")
        
        # Update state
        self.last_variance = current_variance
        self.last_state = current_state
    
    def disconnect(self):
        """Disconnect from MQTT broker"""
        if self.client:
            try:
                self.client.disconnect()
                print('MQTT disconnected')
            except Exception as e:
                print(f"Error disconnecting MQTT: {e}")
    
    def publish_info(self):
        """Publish system info"""
        if self.cmd_handler:
            self.cmd_handler.cmd_info()
