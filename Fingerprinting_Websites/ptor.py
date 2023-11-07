from selenium import webdriver
from selenium.webdriver.firefox.options import Options
from selenium.webdriver.common.proxy import Proxy, ProxyType

proxy = Proxy({
    'proxyType': ProxyType.MANUAL,
    'socksProxy': '127.0.0.1:9050',
    'socksVersion': 5,
})

options = Options()
options.proxy = proxy 
options.binary_location = '/home/xin/website-fingerprinting/tor-browser/Browser/firefox'  # doesn't work
#options.binary_location = '/path/to/normal/firefox'  # works

driver = webdriver.Firefox(options=options)  #  use path to standard `Firefox`/home/xin/website-fingerprinting/tor-browser/Browser


url = 'https://www.google.com/'
url = 'https://icanhazip.com'     # it shows your IP
#url = 'https://httpbin.org/get'  # it shows your IP and headers/cookies

driver.get(url)

