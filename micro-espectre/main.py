import gc
gc.collect()
import network
gc.collect()
try:
    _wlan = network.WLAN()
except Exception:
    _wlan = None
gc.collect()
import src.main
gc.collect()
# Pass pre-created WLAN to main (avoids create failure on S3)
src.main.main(_wlan)
