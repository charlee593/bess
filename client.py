import socket
import os
import scapy.all as scapy


# Data pkt
class MDCData(scapy.Packet):
    name = 'MulticastDataCenterData '
    fields_desc=[scapy.ShortField('addr', 0),
                 scapy.XByteField('mode', 0),
                 scapy.XByteField('label' , 0),
                 scapy.ShortField('appID' , 0),
                 scapy.ShortField('dataID' , 0),
                 scapy.ShortField('sn' , 0)]




data_eth = scapy.Ether(src='aa:bb:cc:dd:ee:01', dst='11:22:33:44:55:01', type=0x0800)
data_ip = scapy.IP(src='10.1.0.1', dst='10.0.0.1')
data_udp = scapy.UDP(sport=10001, dport=10002)
unlabeled_data_mdc = MDCData(addr=0x1a1b, mode=0x01, label=0x0)

unlabeled_data_pkt = data_eth/data_ip/data_udp/unlabeled_data_mdc

print("Connecting...")
if os.path.exists("/tmp/mdc_dp_p.sock"):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    client.setblocking(0)
    client.connect("/tmp/mdc_dp_p.sock")
    print("Ready.")

    sending_pk_sn = 0
    recv_cnt = 0

    while sending_pk_sn < 100:
        try:
            unlabeled_data_pkt[MDCData].sn = sending_pk_sn
            client.send(bytes(unlabeled_data_pkt))
            sending_pk_sn += 1

        except KeyboardInterrupt as k:
            print("Shutting down.")
            client.close()
            break


    while True:
        try:
            data = client.recv(len(bytes(unlabeled_data_pkt)))

            datass = sniff(filter="MulticastDataCenterData", count=len(bytes(unlabeled_data_pkt)))

            print(datass.summary())

            if len(data) > 0:
                recv_cnt += 1
                print("S ", sending_pk_sn, "R ", recv_cnt)

        except KeyboardInterrupt as k:
            print("Shutting down.")
            client.close()
            break

        except IOError as e:  # and here it is handeled
            pass


else:
    print("Couldn't Connect!")
    print("Done")
