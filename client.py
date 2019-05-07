import socket
import os
import scapy.all as scapy
import struct

# Data pkt
class MDCData(scapy.Packet):
    name = 'MulticastDataCenterData '
    fields_desc=[scapy.ShortField('addr', 0),
                 scapy.XByteField('mode', 0),
                 scapy.XByteField('label' , 0),
                 scapy.ByteEnumField("code", 5, {1:"REQUEST",2:"PTCH_REQ",3:"DATA_FNSD",4:"PTCH_DATA", 5:"DATA"}),
                 scapy.XByteField('appID' , 2),
                 scapy.XByteField('dataID' , 0),
                 scapy.XByteField('sn' , 0),
                 scapy.XByteField('dataSize' , 99)]




data_eth = scapy.Ether(src='aa:bb:cc:dd:ee:01', dst='11:22:33:44:55:01', type=0x0800)
print("Size of data_eth:", len(bytes(data_eth)))
data_ip = scapy.IP(src='10.1.0.1', dst='10.0.0.1')
print("Size of data_ip:", len(bytes(data_ip)))
data_udp = scapy.UDP(sport=10001, dport=10002)
print("Size of data_udp:", len(bytes(data_udp)))
unlabeled_data_mdc = MDCData(addr=0x1a1b, mode=0x02, label=0x01)
print("Size of unlabeled_data_mdc:", len(bytes(unlabeled_data_mdc)))

unlabeled_data_pkt = data_eth/data_ip/data_udp/unlabeled_data_mdc
print("Sendiing unlabeled_data_mdc_header: ", bytes(unlabeled_data_mdc))
print("Size of unlabeled_data_pkt:", len(bytes(unlabeled_data_pkt)))

print("Connecting...")
if os.path.exists("/tmp/mdc_dp_p.sock"):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    client.setblocking(0)
    client.connect("/tmp/mdc_dp_p.sock")
    print("Ready.")

    sending_pk_sn = 0
    recv_cnt = 0

    # while sending_pk_sn < 10:
    #     try:
    #         unlabeled_data_pkt[MDCData].sn = sending_pk_sn
    #         # print("Sendiing unlabeled_data_mdc_header: ", bytes(unlabeled_data_pkt[MDCData]).encode('hex'))
    #         # print(bytes(unlabeled_data_pkt).encode('hex'))
    #         client.send(bytes(unlabeled_data_pkt))
    #         sending_pk_sn += 1

    #     except KeyboardInterrupt as k:
    #         print("Shutting down.")
    #         client.close()
    #         break


    while True:
        try:
            data = client.recv(len(bytes(unlabeled_data_pkt)))

            if len(data) > 0:
                data_eth_header = data[:len(bytes(data_eth))]
                data_ip_header = data[len(bytes(data_eth)):len(bytes(data_eth))+len(bytes(data_ip))]
                data_udp_header = data[len(bytes(data_eth))+len(bytes(data_ip)):len(bytes(data_eth))+len(bytes(data_ip))+len(bytes(data_udp))]
                unlabeled_data_mdc_header = data[len(bytes(data_eth))+len(bytes(data_ip))+len(bytes(data_udp)):]
                print("unlabeled_data_mdc_header: ", unlabeled_data_mdc_header)

                print("unlabeled_data_mdc_header: ", struct.unpack('>B', unlabeled_data_mdc_header[8:9]), " ", unlabeled_data_mdc_header[8:9])
                print("unlabeled_data_mdc_header addr: ", struct.unpack('>H', unlabeled_data_mdc_header[:2]), " ", unlabeled_data_mdc_header[:2])
                print("unlabeled_data_mdc_header mode: ", struct.unpack('>B', unlabeled_data_mdc_header[2:3]), " ", unlabeled_data_mdc_header[2:3])

                sending_pk_sn = 0

                recv_cnt += 1
                print("S ", sending_pk_sn, "R ", recv_cnt)
            else:
                while sending_pk_sn < 100:
                    try:
                        unlabeled_data_pkt[MDCData].sn = sending_pk_sn
                        unlabeled_data_pkt[MDCData].code = 4
                        # print("Sendiing unlabeled_data_mdc_header: ", bytes(unlabeled_data_pkt[MDCData]).encode('hex'))
                        # print(bytes(unlabeled_data_pkt).encode('hex'))
                        client.send(bytes(unlabeled_data_pkt))
                        sending_pk_sn += 1

                    except KeyboardInterrupt as k:
                        print("Shutting down.")
                        client.close()
                        break

        except KeyboardInterrupt as k:
            print("Shutting down.")
            client.close()
            break

        except IOError as e:  # and here it is handeled
            pass


else:
    print("Couldn't Connect!")
    print("Done")
