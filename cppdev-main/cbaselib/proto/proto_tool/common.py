import os
import socket
import xml.etree.ElementTree as ET


def loginfo(*argv):
    #print(datetime.datetime.now(), argv)
    print(argv)
def openxml(path):
	if not os.path.exists(path):
		loginfo("ERROR: not find:" + path)
		return None

	loginfo("open xml file path:" +  path)
	tree=ET.parse(path)
	return tree

def gethostip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
        loginfo("ip=",ip)
    finally:
        s.close()
    #return "172.17.50.181"
    return ip

def getlocalip():
    global local_ip
    if local_ip == '':
        local_ip = gethostip()
        loginfo("getlocal_ip=" , local_ip)
    return local_ip