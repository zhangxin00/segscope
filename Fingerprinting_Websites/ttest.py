from stem.control import Controller
from tbselenium.tbdriver import TorBrowserDriver
import tbselenium.common as cm
from tbselenium.utils import launch_tbb_tor_with_stem
from selenium.webdriver.common.utils import free_port
import tempfile
from os.path import join
import time


tbb_dir = "/home/xin/website-fingerprinting/tor-browser/"
gecko = "/home/xin/geckodriver"

socks_port = free_port()
control_port = free_port()
tor_data_dir = tempfile.mkdtemp()
torrc = {'ControlPort': str(control_port),
        'SOCKSPort': str(socks_port),
        'DataDirectory': tor_data_dir}
tor_binary = join(tbb_dir, cm.DEFAULT_TOR_BINARY_PATH)
tor_process = launch_tbb_tor_with_stem(tbb_path=tbb_dir, torrc=torrc, tor_binary=tor_binary)

Controller.from_port(port=control_port).authenticate()
driver = TorBrowserDriver(tbb_dir, socks_port=socks_port, control_port=control_port, tor_cfg=cm.USE_STEM, executable_path=gecko)
driver.load_url("https://check.torproject.org")
time.sleep(1)
tor_process.kill()
