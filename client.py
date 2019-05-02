import socket
import os
import scapy.all as scapy


CONST_DATA_SIZE = 46

num_cores = int($BESS_CORES!'3')
data_pkt_size = int($BESS_DATA_PKT_SIZE!'1024')

assert 64 <= data_pkt_size <= 1024, 'Data pkts needs to be in the range [64, 1024] in bytes'

print('Core count = %d' % num_cores)
print('Data pkt size = %d' % data_pkt_size)

# Data pkt
class MDCData(scapy.Packet):
    name = 'MulticastDataCenterData '
    fields_desc=[scapy.ShortField('addr', 0),
                 scapy.XByteField('mode', 0),
                 scapy.XByteField('label' , 0)]


# Data pkt template
data_eth = scapy.Ether(src='aa:bb:cc:dd:ee:01', dst='11:22:33:44:55:01', type=0x0800)
data_ip = scapy.IP(src='10.1.0.1', dst='10.0.0.1')
data_udp = scapy.UDP(sport=10001, dport=10002)
unlabeled_data_mdc = MDCData(addr=0x1a1b, mode=0x00, label=0x0)
data_payload = bytes(unlabeled_data_mdc) + b"\0" * (data_pkt_size-CONST_DATA_SIZE)
unlabeled_data_pkt = data_eth/data_ip/data_udp/data_payload
unlabeled_data_pkt_bytes = bytes(unlabeled_data_pkt)


print("Connecting...")
if os.path.exists("/tmp/mdc_dp_p.sock"):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    client.connect("/tmp/mdc_dp_p.sock")
    print("Ready.")
    print("Ctrl-C to quit.")
    print("Sending 'DONE' shuts down the server and quits.")
    while True:
        try:
          client.send(unlabeled_data_pkt_bytes)
        except KeyboardInterrupt as k:
            print("Shutting down.")
            client.close()
            break
else:
    print("Couldn't Connect!")
    print("Done")
