import os
import socket
import xml.etree.ElementTree as ET


def loginfo(*argv):
    #print(datetime.datetime.now(), argv)
    print(argv)

def write_utf8(path, text):
    # 生成结果固定 UTF-8、无 BOM、LF。不要用系统代码页。
    if isinstance(text, bytes):
        text = text.decode("utf-8")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    with open(path, "wb") as fp:
        fp.write(text.encode("utf-8"))
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