
from selenium import webdriver
from selenium.webdriver.firefox.firefox_profile import FirefoxProfile
import os

torexe = os.popen('/home/xin/website-fingerprinting/tor-browser/Browser/firefox')
profile = FirefoxProfile('/home/xin/website-fingerprinting/tor-browser/Browser/TorBrowser/Data/Browser/profile.default')
profile.set_preference('network.proxy.type', 1)
profile.set_preference('network.proxy.socks', '127.0.0.1')
profile.set_preference('network.proxy.socks_port', 9050)
profile.set_preference("network.proxy.socks_remote_dns", False)
profile.update_preferences()
driver = webdriver.Firefox(firefox_profile= profile, executable_path='/home/xin/geckodriver')
# driver = webdriver.Firefox(firefox_profile= profile, )
driver.get("http://check.torproject.org")
driver.get("https://www.amazon.jp/")

