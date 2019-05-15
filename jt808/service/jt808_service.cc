#include "service/jt808_service.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "bcd/bcd.h"
#include "unix_socket/unix_socket.h"


static int EpollRegister(const int &epoll_fd, const int &fd) {
  struct epoll_event ev;
  int ret;
  int flags;

  // important: make the fd non-blocking.
  flags = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  ev.events = EPOLLIN;
  //ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = fd;
  do {
      ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  } while (ret < 0 && errno == EINTR);

  return ret;
}

static int EpollUnregister(const int &epoll_fd, const int &fd) {
  int ret;

  do {
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
  } while (ret < 0 && errno == EINTR);

  return ret;
}

static inline uint16_t EndianSwap16(const uint16_t &value) {
  assert(sizeof(value) == 2);
  return (((value & 0xff) << 8) | (value >> 8));
}

static inline uint32_t EndianSwap32(const uint32_t &value) {
  assert(sizeof(value) == 4);
  return ((value >> 24) |
          ((value & 0x00ff0000) >> 8) |
          ((value & 0x0000ff00) << 8) |
          (value << 24));
}

static uint8_t BccCheckSum(const uint8_t *src, const int &len) {
  uint8_t checksum = 0;
  for (int i = 0; i < len; ++i) {
    checksum = checksum ^ src[i];
  }
  return checksum;
}

static uint16_t Escape(uint8_t *src, const int &len) {
  uint16_t i;
  uint16_t j;
  uint8_t *buffer = new uint8_t[len * 2];

  memset(buffer, 0x0, len * 2);
  for (i = 0, j = 0; i < len; ++i) {
    if (src[i] == PROTOCOL_SIGN) {
      buffer[j++] = PROTOCOL_ESCAPE;
      buffer[j++] = PROTOCOL_ESCAPE_SIGN;
    } else if(src[i] == PROTOCOL_ESCAPE) {
      buffer[j++] = PROTOCOL_ESCAPE;
      buffer[j++] = PROTOCOL_ESCAPE_ESCAPE;
    } else {
      buffer[j++] = src[i];
    }
  }

  memcpy(src, buffer, j);
  delete [] buffer;
  return j;
}

static uint16_t ReverseEscape(uint8_t *src, const int &len) {
  uint16_t i;
  uint16_t j;
  uint8_t *buffer = new uint8_t[len];

  memset(buffer, 0x0, len);
  for (i = 0, j = 0; i < len; ++i) {
    if ((src[i] == PROTOCOL_ESCAPE) && (src[i+1] == PROTOCOL_ESCAPE_SIGN)) {
      buffer[j++] = PROTOCOL_SIGN;
      ++i;
    } else if ((src[i] == PROTOCOL_ESCAPE) &&
               (src[i+1] == PROTOCOL_ESCAPE_ESCAPE)) {
      buffer[j++] = PROTOCOL_ESCAPE;
      ++i;
    } else {
      buffer[j++] = src[i];
    }
  }

  memcpy(src, buffer, j);
  delete [] buffer;
  return j;
}

static inline void PreparePhoneNum(const char *src, uint8_t *bcd_array) {
  char phone_num[6] = {0};
  BcdFromStringCompress(src, phone_num, strlen(src));
  memcpy(bcd_array, phone_num, 6);
}

static void ParsePositionReport(const MessageData &msg) {
  uint16_t u16val;
  uint32_t u32val;
  double latitude;
  double longitude;
  float altitude;
  float speed;
  float bearing;
  AlarmBit alarm_bit = {0};
  StatusBit status_bit = {0};
  char timestamp[6];
  char phone_num[6] = {0};
  char device[12] = {0};

  memcpy(phone_num, &msg.buffer[5], 6);
  StringFromBcdCompress(phone_num, device, 6);
  memcpy(&u32val, &msg.buffer[13], 4);
  alarm_bit.value = EndianSwap32(u32val);
  memcpy(&u32val, &msg.buffer[17], 4);
  status_bit.value = EndianSwap32(u32val);
  memcpy(&u32val, &msg.buffer[21], 4);
  longitude = EndianSwap32(u32val) / 1000000.0;
  memcpy(&u32val, &msg.buffer[25], 4);
  latitude = EndianSwap32(u32val) / 1000000.0;
  memcpy(&u16val, &msg.buffer[29], 2);
  altitude = EndianSwap16(u16val);
  memcpy(&u16val, &msg.buffer[31], 2);
  speed = EndianSwap16(u16val) * 10.0;
  memcpy(&u16val, &msg.buffer[33], 2);
  bearing = EndianSwap16(u16val);
  timestamp[0] = HexFromBcd(msg.buffer[35]);
  timestamp[1] = HexFromBcd(msg.buffer[36]);
  timestamp[2] = HexFromBcd(msg.buffer[37]);
  timestamp[3] = HexFromBcd(msg.buffer[38]);
  timestamp[4] = HexFromBcd(msg.buffer[39]);
  timestamp[5] = HexFromBcd(msg.buffer[40]);
  fprintf(stdout, "\tdevice: %s\n"
                  "\talarm flags: %08X\n""\tstatus flags: %08X\n"
                  "\tlongitude: %lf%c\n"
                  "\tlatitude: %lf%c\n"
                  "\taltitude: %f\n"
                  "\tspeed: %f\n""\tbearing: %f\n"
                  "\ttimestamp: 20%02d-%02d-%02d, %02d:%02d:%02d\n",
          device, alarm_bit.value, status_bit.value,
          longitude, status_bit.bit.ewlongitude == 0 ? 'E':'W',
          latitude, status_bit.bit.snlatitude == 0 ? 'N':'S',
          altitude,
          speed, bearing,
          timestamp[0], timestamp[1], timestamp[2],
          timestamp[3], timestamp[4], timestamp[5]);
}

static uint8_t GetParameterTypeByParameterId(const uint32_t &para_id) {
  switch (para_id) {
    case GNSSPOSITIONMODE: case GNSSBAUDERATE: case GNSSOUTPUTFREQ:
    case GNSSUPLOADWAY: case STARTUPGPS: case STARTUPCDRADIO:
    case STARTUPNTRIPCORS: case STARTUPNTRIPSERV: case STARTUPJT808SERV:
    case GPSLOGGGA: case GPSLOGRMC: case GPSLOGATT:
    case CDRADIORECEIVEMODE: case CDRADIOFORMCODE: case NTRIPCORSREPORTINTERVAL:
    case NTRIPSERVICEREPORTINTERVAL: case JT808SERVICEREPORTINTERVAL:
      return kByteType;
    case CAN1UPLOADINTERVAL: case CAN2UPLOADINTERVAL: case CDRADIOWORKINGFREQ:
    case NTRIPCORSPORT: case NTRIPSERVICEPORT: case JT808SERVICEPORT:
      return kWordType;
    case HEARTBEATINTERVAL: case TCPRESPONDTIMEOUT: case TCPMSGRETRANSTIMES:
    case UDPRESPONDTIMEOUT: case UDPMSGRETRANSTIMES: case SMSRESPONDTIMEOUT:
    case SMSMSGRETRANSTIMES: case POSITIONREPORTWAY: case POSITIONREPORTPLAN:
    case NOTLOGINREPORTTIMEINTERVAL: case SLEEPREPORTTIMEINTERVAL:
    case ALARMREPORTTIMEINTERVAL: case DEFTIMEREPORTTIMEINTERVAL:
    case NOTLOGINREPORTDISTANCEINTERVAL: case SLEEPREPORTDISTANCEINTERVAL:
    case ALARMREPORTDISTANCEINTERVAL: case DEFTIMEREPORTDISTANCEINTERVAL:
    case INFLECTIONPOINTRETRANSANGLE: case ALARMSHIELDWORD: case ALARMSENDTXT:
    case ALARMSHOOTSWITCH: case ALARMSHOOTSAVEFLAGS: case ALARMKEYFLAGS:
    case MAXSPEED: case GNSSOUTPUTCOLLECTFREQ: case GNSSUPLOADSET:
    case CAN1COLLECTINTERVAL: case CAN2COLLECTINTERVAL: case CDRADIOBAUDERATE:
      return kDwordType;
    case CANSPECIALSET: case NTRIPCORSIP: case NTRIPCORSUSERNAME:
    case NTRIPCORSPASSWD: case NTRIPCORSMOUNTPOINT: case NTRIPSERVICEIP:
    case NTRIPSERVICEUSERNAME: case NTRIPSERVICEPASSWD:
    case NTRIPSERVICEMOUNTPOINT: case JT808SERVICEIP: case JT808SERVICEPHONENUM:
      return kStringType;
    default:
      return kUnknowType;
  }
}

static uint8_t GetParameterLengthByParameterType(const uint8_t &para_type) {
  switch (para_type) {
    case kByteType:
      return 1;
    case kWordType:
      return 2;
    case kDwordType:
      return 4;
    case kStringType:
    case kUnknowType:
    default:
      return 0;
   }
}

static void AddParameterNodeIntoList(std::list<TerminalParameter *> *para_list,
                                     const uint32_t &para_id,
                                     const char *para_value) {
  if (para_list == nullptr) {
    return ;
  }

  TerminalParameter *node = new TerminalParameter;
  memset(node, 0x0, sizeof(*node));
  node->parameter_id = para_id;
  node->parameter_type = GetParameterTypeByParameterId(para_id);
  node->parameter_len = GetParameterLengthByParameterType(
                             node->parameter_type);
  if (para_value != nullptr) {
    if (node->parameter_type == kStringType) {
      node->parameter_len = strlen(para_value);
    }
    memcpy(node->parameter_value, para_value, node->parameter_len);
  }
  para_list->push_back(node);
}

template <class T>
static void ClearListElement(std::list<T *> &list) {
  if (list.empty()) {
    return ;
  }

  auto element_it = list.begin();
  while (element_it != list.end()) {
    delete (*element_it);
    element_it = list.erase(element_it);
  }
}

template <class T>
static void ClearListElement(std::list<T *> *list) {
  if (list == nullptr || list->empty()) {
    return ;
  }

  auto element_it = list->begin();
  while (element_it != list->end()) {
    delete (*element_it);
    element_it = list->erase(element_it);
  }

  delete list;
  list = nullptr;
}

static void ReadDevicesList(const char *path, std::list<DeviceNode> &list) {
  char *result;
  char line[128] = {0};
  char flags = ';';
  uint32_t u32val;
  std::ifstream ifs;

  ifs.open(path, std::ios::in | std::ios::binary);
  if (ifs.is_open()) {
    std::string str;
    while (getline(ifs, str)) {
      DeviceNode node = {0};
      memset(line, 0x0, sizeof(line));
      str.copy(line, str.length(), 0);
      result = strtok(line, &flags);
      str = result;
      str.copy(node.phone_num, str.size(), 0);
      result = strtok(NULL, &flags);
      str = result;
      u32val = 0;
      sscanf(str.c_str(), "%u", &u32val);
      memcpy(node.authen_code, &u32val, 4);
      node.socket_fd = -1;
      list.push_back(node);
    }
    ifs.close();
  }
}

Jt808Service::Jt808Service() {
  listen_sock_ = 0;
  epoll_fd_ = 0;
  max_count_ = 0;
  message_flow_num_ = 0;
  epoll_events_ = nullptr;
}

Jt808Service::~Jt808Service() {
  if(listen_sock_ > 0){
    close(listen_sock_);
  }
  if(epoll_fd_ > 0){
    close(epoll_fd_);
  }
  device_list_.clear();
  delete [] epoll_events_;
}

bool Jt808Service::Init(const int &port, const int &max_count) {
  max_count_ = max_count;
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(struct sockaddr_in));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock_ == -1) {
    exit(1);
  }

  if (bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&server_addr),
           sizeof(struct sockaddr)) == -1) {
    exit(1);
  }

  if (listen(listen_sock_, 5) == -1) {
    exit(1);
  }

  epoll_events_ = new struct epoll_event[max_count_];
  if (epoll_events_ == NULL) {
    exit(1);
  }

  epoll_fd_ = epoll_create(max_count_);
  EpollRegister(epoll_fd_, listen_sock_);

  ReadDevicesList(kDevicesFilePath, device_list_);

  socket_fd_ = ServerListen(kCommandInterfacePath);
  EpollRegister(epoll_fd_, socket_fd_);

  return true;
}

bool Jt808Service::Init(const char *ip, const int &port, const int &max_count) {
  max_count_ = max_count;
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(struct sockaddr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = inet_addr(ip);

  listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock_ == -1) {
    exit(1);
  }

  if (bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&server_addr),
           sizeof(struct sockaddr)) == -1) {
    exit(1);
  }

  if (listen(listen_sock_, 5) == -1) {
    exit(1);
  }

  epoll_events_ = new struct epoll_event[max_count_];
  if (epoll_events_ == NULL) {
    exit(1);
  }

  epoll_fd_ = epoll_create(max_count_);
  EpollRegister(epoll_fd_, listen_sock_);

  ReadDevicesList(kDevicesFilePath, device_list_);

  socket_fd_ = ServerListen(kCommandInterfacePath);
  EpollRegister(epoll_fd_, socket_fd_);

  return true;
}

int Jt808Service::AcceptNewClient(void) {
  uint16_t command = 0;
  struct sockaddr_in client_addr;
  char phone_num[6] = {0};
  decltype (device_list_.begin()) device_it;
  ProtocolParameters propara = {0};
  MessageData msg = {0};

  memset(&client_addr, 0, sizeof(struct sockaddr_in));
  socklen_t clilen = sizeof(struct sockaddr);
  int new_sock = accept(listen_sock_,
                        reinterpret_cast<struct sockaddr*>(&client_addr),
                        &clilen);

  int keepalive = 1;  // enable keepalive attributes.
  int keepidle = 30;  // time out for starting detection.
  int keepinterval = 5;  // time interval for sending packets during detection.
  int keepcount = 3;  // max times for sending packets during detection.
  setsockopt(new_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(new_sock, SOL_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
  setsockopt(new_sock, SOL_TCP, TCP_KEEPINTVL,
             &keepinterval, sizeof(keepinterval));
  setsockopt(new_sock, SOL_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount));

  if (!RecvFrameData(new_sock, message_)) {
    memset(&propara, 0x0, sizeof(propara));
    command = Jt808FrameParse(message_, propara);
    switch (command) {
      case UP_REGISTER:
        memset(msg.buffer, 0x0, MAX_PROFRAMEBUF_LEN);
        msg.len = Jt808FramePack(msg, DOWN_REGISTERRSPONSE, propara);
        SendFrameData(new_sock, msg);
        if (propara.respond_result != kSuccess) {
          close(new_sock);
          new_sock = -1;
          break;
        }

        if (!RecvFrameData(new_sock, msg)) {
          command = Jt808FrameParse(msg, propara);
          if (command != UP_AUTHENTICATION) {
            close(new_sock);
            new_sock = -1;
            break;
          }
        }
      case UP_AUTHENTICATION:
        memset(msg.buffer, 0x0, MAX_PROFRAMEBUF_LEN);
        msg.len = Jt808FramePack(msg, DOWN_UNIRESPONSE, propara);
        SendFrameData(new_sock, msg);
        if (propara.respond_result != kSuccess) {
          close(new_sock);
          new_sock = -1;
        } else if (!device_list_.empty()) {
          device_it = device_list_.begin();
          while (device_it != device_list_.end()) {
            BcdFromStringCompress(device_it->phone_num,
                                  phone_num, strlen(device_it->phone_num));
            if (memcmp(phone_num, propara.phone_num, 6) == 0) {
              break;
            }
            ++device_it;
          }
          if (device_it != device_list_.end()) {
            memcpy(device_it->manufacturer_id, propara.manufacturer_id, 5);
            device_it->socket_fd = new_sock;
          }
        }
        break;
      default:
        close(new_sock);
        new_sock = -1;
        break;
    }
  }

  if (new_sock > 0) {
    EpollRegister(epoll_fd_, new_sock);
  }

  return new_sock;
}

void Jt808Service::Run(const int &time_out) {
  int ret = -1;
  int i;
  int active_count;
  char recv_buff[65536] = {0};
  ProtocolParameters propara = {0};
  MessageData msg = {0};

  while (1) {
    ret = Jt808ServiceWait(time_out);
    if (ret == 0) {  // epoll time out.
      continue;
    } else {
      active_count = ret;
      for (i = 0; i < active_count; ++i) {
        if (epoll_events_[i].data.fd == listen_sock_) {
          if (epoll_events_[i].events & EPOLLIN) {
            AcceptNewClient();
          }
        } else if (epoll_events_[i].data.fd == socket_fd_) {
          if (epoll_events_[i].events & EPOLLIN) {
            AcceptNewCommandClient();
            EpollRegister(epoll_fd_, client_fd_);
          }
        } else if (epoll_events_[i].data.fd == client_fd_) {
          memset(recv_buff, 0x0, sizeof(recv_buff));
          if ((ret = recv(client_fd_, recv_buff, sizeof(recv_buff), 0)) > 0) {
            if (ParseCommand(recv_buff) >= 0) {
               send(client_fd_, recv_buff, strlen(recv_buff), 0);
            }
          }
          close(client_fd_);
        } else if ((epoll_events_[i].events & EPOLLIN) &&
                   !device_list_.empty()) {
          auto device_it = device_list_.begin();
          while (device_it != device_list_.end()) {
            if (epoll_events_[i].data.fd == device_it->socket_fd) {
              if (!RecvFrameData(epoll_events_[i].data.fd, msg)) {
                int cmd = Jt808FrameParse(msg, propara);
                switch (cmd) {
                  case UP_UPDATERESULT: case UP_POSITIONREPORT:
                    memset(msg.buffer, 0x0, sizeof(msg.buffer));
                    msg.len = Jt808FramePack(msg, DOWN_UNIRESPONSE, propara);
                    if (SendFrameData(epoll_events_[i].data.fd, msg)) {
                      EpollUnregister(epoll_fd_, epoll_events_[i].data.fd);
                      close(epoll_events_[i].data.fd);
                      device_it->socket_fd = -1;
                    }
                    break;
                  default:
                    break;
                }
              } else {
                EpollUnregister(epoll_fd_, epoll_events_[i].data.fd);
                close(epoll_events_[i].data.fd);
                device_it->socket_fd = -1;
              }
              break;
            }
            ++device_it;
          }
        }
      }
    }
  }
}

int Jt808Service::SendFrameData(const int &fd, const MessageData &msg) {
  int ret = -1;

  signal(SIGPIPE, SIG_IGN);
  ret = send(fd, msg.buffer, msg.len, 0);
  if (ret < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
      ret = 0;
    } else {
      if (errno == EPIPE) {
        printf("%s[%d]: remote socket close!!!\n", __FILE__, __LINE__);
      }
      ret = -1;
      printf("%s[%d]: send data failed!!!\n", __FILE__, __LINE__);
    }
  } else if (ret == 0) {
    printf("%s[%d]: connection disconect!!!\n", __FILE__, __LINE__);
    ret = -1;
  }

  return ret >= 0 ? 0 : ret;
}

int Jt808Service::RecvFrameData(const int &fd, MessageData &msg) {
  int ret = -1;

  memset(msg.buffer, 0x0, MAX_PROFRAMEBUF_LEN);
  ret = recv(fd, msg.buffer, MAX_PROFRAMEBUF_LEN, 0);
  if (ret < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)) {
      ret = 0;
    } else {
      ret = -1;
      printf("%s[%d]: recv data failed!!!\n", __FILE__, __LINE__);
    }
    msg.len = 0;
  } else if(ret == 0) {
    ret = -1;
    msg.len = 0;
    printf("%s[%d]: connection disconect!!!\n", __FILE__, __LINE__);
  } else {
    msg.len = ret;
  }

  return ret >= 0 ? 0 : ret;
}

int Jt808Service::Jt808FramePack(MessageData &msg,
                                 const uint16_t &command,
                                 const ProtocolParameters &propara) {
  uint8_t *msg_body;
  uint16_t u16val;
  uint32_t u32val;
  MessageHead *msghead_ptr;

  msghead_ptr = (MessageHead *)&msg.buffer[1];
  msghead_ptr->id = EndianSwap16(command);
  msghead_ptr->attribute.val = 0x0000;
  msghead_ptr->attribute.bit.encrypt = 0;
  message_flow_num_++;
  msghead_ptr->msgflownum = EndianSwap16(message_flow_num_);
  memcpy(msghead_ptr->phone, propara.phone_num, 6);
  msghead_ptr->attribute.bit.package = 0;
  msg_body = &msg.buffer[MSGBODY_NOPACKAGE_POS];
  msg.len = 13;

  switch (command) {
    case DOWN_UNIRESPONSE:
      u16val = EndianSwap16(propara.respond_flow_num);
      memcpy(msg_body, &u16val, 2);
      msg_body += 2;
      u16val = EndianSwap16(propara.respond_id);
      memcpy(msg_body, &u16val, 2);
      msg_body += 2;
      *msg_body = propara.respond_result;
      msg_body++;
      msghead_ptr->attribute.bit.msglen = 5;
      msghead_ptr->attribute.val = EndianSwap16(msghead_ptr->attribute.val);
      msg.len += 5;
      break;
    case DOWN_REGISTERRSPONSE:
      u16val = EndianSwap16(propara.respond_flow_num);
      memcpy(msg_body, &u16val, 2);
      msg_body += 2;
      *msg_body = propara.respond_result;
      msg_body++;
      if (propara.respond_result == kSuccess) {
        // send authen code if register success.
        memcpy(msg_body, propara.authen_code, 4);
        msg_body += 4;
        msghead_ptr->attribute.bit.msglen = 7;
        msg.len += 7;
      } else {
        msghead_ptr->attribute.bit.msglen = 3;
        msg.len += 3;
      }
      msghead_ptr->attribute.val = EndianSwap16(msghead_ptr->attribute.val);
      break;
    case DOWN_SETTERMPARA:
      if (propara.packet_total_num > 1) {
        msghead_ptr->attribute.bit.package = 1;
        u16val = EndianSwap16(propara.packet_total_num);
        memcpy(&msg.buffer[13], &u16val, 2);
        u16val = EndianSwap16(propara.packet_sequence_num);
        memcpy(&msg.buffer[15], &u16val, 2);
        msg_body += 4;
        msg.len += 4;
      }
      if ((propara.terminal_parameter_list != nullptr) &&
          !propara.terminal_parameter_list->empty()) {
        msg_body[0] = propara.terminal_parameter_list->size();
        msg_body++;
        msg.len++;
        msghead_ptr->attribute.bit.msglen = 1;
        auto paralist_it = propara.terminal_parameter_list->begin();
        while (paralist_it != propara.terminal_parameter_list->end()) {
          u32val = EndianSwap32((*paralist_it)->parameter_id);
          memcpy(msg_body, &u32val, 4);
          msg_body += 4;
          memcpy(msg_body, &((*paralist_it)->parameter_len), 1);
          msg_body++;
          switch (GetParameterTypeByParameterId((*paralist_it)->parameter_id)) {
            case kWordType:
              memcpy(&u16val, (*paralist_it)->parameter_value, 2);
              u16val = EndianSwap16(u16val);
              memcpy(msg_body, &u16val, 2);
              break;
            case kDwordType:
              memcpy(&u32val, (*paralist_it)->parameter_value, 4);
              u32val = EndianSwap32(u32val);
              memcpy(msg_body, &u32val, 4);
              break;
            case kByteType:
            case kStringType:
              memcpy(msg_body, (*paralist_it)->parameter_value,
                     (*paralist_it)->parameter_len);
              break;
            default:
              break;
          }
          msg_body += (*paralist_it)->parameter_len;
          msg.len += 5+(*paralist_it)->parameter_len;
          msghead_ptr->attribute.bit.msglen += 5+(*paralist_it)->parameter_len;
          ++paralist_it;
        }
      } else {
        msg_body[0] = 0;
        msg_body++;
        msg.len++;
        msghead_ptr->attribute.bit.msglen = 1;
      }
      msghead_ptr->attribute.val = EndianSwap16(msghead_ptr->attribute.val);
      break;
    case DOWN_GETTERMPARA:
      break;
    case DOWN_GETSPECTERMPARA:
      *msg_body = propara.terminal_parameter_id_count;
      msg_body++;
      msg.len++;
      msghead_ptr->attribute.bit.msglen = 1;
      // deal terminal parameters id.
      for (int i = 0; i < propara.terminal_parameter_id_count; ++i) {
          memcpy(msg_body, propara.terminal_parameter_id_buffer + i*4, 4);
          msg_body += 4;
          msg.len += 4;
          msghead_ptr->attribute.bit.msglen += 4;
      }
      msghead_ptr->attribute.val = EndianSwap16(msghead_ptr->attribute.val);
      break;
    case DOWN_UPDATEPACKAGE:
      if (propara.packet_total_num > 1) {
        msghead_ptr->attribute.bit.package = 1;
        u16val = propara.packet_total_num;
        u16val = EndianSwap16(u16val);
        memcpy(&msg.buffer[13], &u16val, 2);
        u16val = propara.packet_sequence_num;
        u16val = EndianSwap16(u16val);
        memcpy(&msg.buffer[15], &u16val, 2);
        msg_body += 4;
        msg.len += 4;
      }
      *msg_body = propara.upgrade_type;
      msg_body++;
      memcpy(msg_body, propara.manufacturer_id, 5);
      msg_body += 5;
      msg.len += 6;
      *msg_body = propara.version_num_len;
      msg_body++;
      msg.len++;
      memcpy(msg_body, propara.version_num, propara.version_num_len);
      msg_body += propara.version_num_len;
      msg.len += propara.version_num_len;
      u32val = EndianSwap32(propara.packet_data_len);
      memcpy(msg_body, &u32val, 4);
      msg_body += 4;
      msg.len += 4;
      // content of the upgrade file.
      memcpy(msg_body, propara.packet_data, propara.packet_data_len);
      msg_body += propara.packet_data_len;
      msg.len += propara.packet_data_len;
      msghead_ptr->attribute.bit.msglen = 11 + propara.version_num_len +
                                          propara.packet_data_len;
      msghead_ptr->attribute.val = EndianSwap16(msghead_ptr->attribute.val);
      break;
    default:
      break;
  }

  *msg_body = BccCheckSum(&msg.buffer[1], msg.len - 1);
  msg.len++;

  msg.len = Escape(msg.buffer + 1, msg.len);
  msg.buffer[0] = PROTOCOL_SIGN;
  msg.buffer[msg.len++] = PROTOCOL_SIGN;

  printf("%s[%d]: socket-send:\n", __FILE__, __LINE__);
  for (uint16_t i = 0; i < msg.len; ++i) {
    printf("%02X ", msg.buffer[i]);
  }
  printf("\r\n");

  return msg.len;
}

uint16_t Jt808Service::Jt808FrameParse(MessageData &msg,
                                       ProtocolParameters &propara) {
  uint8_t *msg_body;
  char phone_num[12] = {0};
  uint8_t u8val;
  uint16_t retval;
  uint16_t u16val = 0;
  uint32_t u32val;
  MessageHead *msghead_ptr;

  // printf("%s[%d]: socket-recv:\n", __FILE__, __LINE__);
  // for (uint16_t i = 0; i < msg.len; ++i) {
  //   printf("%02X ", msg.buffer[i]);
  // }
  // printf("\r\n");

  msg.len = ReverseEscape(&msg.buffer[1], msg.len);
  msghead_ptr = reinterpret_cast<MessageHead *>(&msg.buffer[1]);
  msghead_ptr->attribute.val = EndianSwap16(msghead_ptr->attribute.val);
  if (msghead_ptr->attribute.bit.package) {
    msg_body = &msg.buffer[MSGBODY_PACKAGE_POS];
  } else {
    msg_body = &msg.buffer[MSGBODY_NOPACKAGE_POS];
  }

  u16val = EndianSwap16(msghead_ptr->id);
  retval = u16val;
  switch (u16val) {
    case UP_UNIRESPONSE:
      memcpy(&u16val, &msg_body[2], 2);
      propara.respond_id = EndianSwap16(u16val);
      switch(propara.respond_id) {
        case DOWN_UPDATEPACKAGE:
          printf("%s[%d]: received updatepackage respond: ",
                 __FILE__, __LINE__);
          break;
        case DOWN_SETTERMPARA:
          printf("%s[%d]: set terminal parameter respond: ",
                 __FILE__, __LINE__);
          break;
        default:
          break;
      }
      // respond result.
      if (msg_body[4] == kSuccess) {
        printf("normal\r\n");
      } else if(msg_body[4] == kFailure) {
        printf("failed\r\n");
      } else if(msg_body[4] == kMessageHasWrong) {
        printf("message has something wrong\r\n");
      } else if(msg_body[4] == kNotSupport) {
        printf("message not support\r\n");
      }
      break;
    case UP_REGISTER:
      propara.respond_flow_num = EndianSwap16(msghead_ptr->msgflownum);
      memcpy(propara.phone_num, msghead_ptr->phone, 6);
      if (!device_list_.empty()) {
        auto device_it = device_list_.begin();
        while (device_it != device_list_.end()) {
          BcdFromStringCompress(device_it->phone_num,
                                phone_num,
                                strlen(device_it->phone_num));
          if (memcmp(phone_num, msghead_ptr->phone, 6) == 0) {
            break;
          }
          ++device_it;
        }
        if (device_it != device_list_.end()) {
          if (device_it->socket_fd == -1) {
            propara.respond_result = kRegisterSuccess;
            memcpy(propara.authen_code, device_it->authen_code, 4);
            memcpy(propara.manufacturer_id, &msg_body[4], 5);
          } else {
            propara.respond_result = kTerminalHaveBeenRegistered;
          }
        } else {
          propara.respond_result = kNoSuchTerminalInTheDatabase;
        }
      } else {
        propara.respond_result = kNoSuchVehicleInTheDatabase;
      }
      break;
    case UP_AUTHENTICATION:
      propara.respond_flow_num = EndianSwap16(msghead_ptr->msgflownum);
      propara.respond_id = EndianSwap16(msghead_ptr->id);
      memcpy(propara.phone_num, msghead_ptr->phone, 6);
      if (!device_list_.empty()) {
        auto device_it = device_list_.begin();
        while (device_it != device_list_.end()) {
          BcdFromStringCompress(device_it->phone_num,
                                phone_num,
                                strlen(device_it->phone_num));
          if (memcmp(phone_num, msghead_ptr->phone, 6) == 0) {
            break;
          }
          ++device_it;
        }
        if ((device_it != device_list_.end()) &&
            (memcmp(device_it->authen_code, msg_body,
                    msghead_ptr->attribute.bit.msglen) == 0)) {
          propara.respond_result = kSuccess;
        } else {
          propara.respond_result = kFailure;
        }
      } else {
        propara.respond_result = kFailure;
      }
      break;
    case UP_GETPARASPONSE:
      printf("%s[%d]: received get terminal parameter respond\n",
             __FILE__, __LINE__);
      propara.respond_flow_num = EndianSwap16(msghead_ptr->msgflownum);
      propara.respond_id = EndianSwap16(msghead_ptr->id);
      if (msghead_ptr->attribute.bit.package) {
        memcpy(&u16val, &msg.buffer[13], 2);
        propara.packet_total_num = EndianSwap16(u16val);
        memcpy(&u16val, &msg.buffer[15], 2);
        propara.packet_sequence_num = EndianSwap16(u16val);
      }
      msg_body += 3;
      if (propara.terminal_parameter_list != nullptr) {
        int len = msghead_ptr->attribute.bit.msglen - 3;
        char parameter_value[256] ={0};
        uint32_t parameter_id = 0;
        while (len) {
          memcpy(&u32val, &msg_body[0], 4);
          parameter_id = EndianSwap32(u32val);
          msg_body += 4;
          u8val = msg_body[0];
          msg_body++;
          memset(parameter_value, 0x0, sizeof(parameter_value));
          switch (GetParameterTypeByParameterId(parameter_id)) {
            case kWordType:
              memcpy(&u16val, &msg_body[0], u8val);
              u16val = EndianSwap16(u16val);
              memcpy(parameter_value, &u16val, u8val);
              break;
            case kDwordType:
              memcpy(&u32val, &msg_body[0], u8val);
              u32val = EndianSwap32(u32val);
              memcpy(parameter_value, &u32val, u8val);
              break;
            case kByteType:
            case kStringType:
              memcpy(parameter_value, &msg_body[0], u8val);
              break;
            default:
              break;
          }
          AddParameterNodeIntoList(propara.terminal_parameter_list,
                                   parameter_id, parameter_value);
          msg_body += u8val;
          len -= (u8val + 5);
        }
        propara.respond_result = kSuccess;
      }
      break;
    case UP_UPDATERESULT:
      printf("%s[%d]: received updateresult respond: ", __FILE__, __LINE__);
      propara.respond_flow_num = EndianSwap16(msghead_ptr->msgflownum);
      propara.respond_id = EndianSwap16(msghead_ptr->id);
      if (msg_body[4] == 0x00) {
        printf("normal\r\n");
      } else if(msg_body[4] == 0x01) {
        printf("failed\r\n");
      } else if(msg_body[4] == 0x02) {
        printf("message has something wrong\r\n");
      } else if(msg_body[4] == 0x03) {
        printf("message not support\r\n");
      }
      propara.respond_result = kSuccess;
      break;
    case UP_POSITIONREPORT:
      printf("%s[%d]: received position report respond:\n", __FILE__, __LINE__);
      propara.respond_flow_num = EndianSwap16(msghead_ptr->msgflownum);
      propara.respond_id = EndianSwap16(msghead_ptr->id);
      ParsePositionReport(msg);
      propara.respond_result = kSuccess;
    default:
      break;
  }

  return retval;
}

static void PrepareParemeterIdList(std::vector<std::string> &va_vec,
                                   std::vector<uint32_t> &id_vec) {
  char parameter_id[9] = {0};
  std::string arg;
  reverse(id_vec.begin(), id_vec.end());
  while (!id_vec.empty()) {
    snprintf(parameter_id, 5, "%04X", id_vec.back());
    arg = parameter_id;
    va_vec.push_back(arg);
    memset(parameter_id, 0x0, sizeof(parameter_id));
    id_vec.pop_back();
  }
  reverse(va_vec.begin(), va_vec.end());
}

int Jt808Service::DealGetStartupRequest(DeviceNode &device, char *result) {
  int retval = -1;
  char parameter_value[2] = {0};
  uint32_t parameter_id;
  std::string arg;
  std::vector<std::string> va_vec;
  std::vector<uint32_t> id_vec;

  id_vec.push_back(STARTUPGPS);
  id_vec.push_back(STARTUPCDRADIO);
  id_vec.push_back(STARTUPNTRIPCORS);
  id_vec.push_back(STARTUPNTRIPSERV);
  id_vec.push_back(STARTUPJT808SERV);
  PrepareParemeterIdList(va_vec, id_vec);
  if ((retval = DealGetTerminalParameterRequest(device, va_vec)) == 0) {
    std::string str = "startup:";
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      sscanf(arg.c_str(), "%x:%s", &parameter_id, parameter_value);
      if (parameter_value[0] == '1') {
        switch (parameter_id) {
          case STARTUPGPS:
            str += " gps";
            break;
          case STARTUPCDRADIO:
            str += " cdradio";
            break;
          case STARTUPNTRIPCORS:
            str += " ntripcors";
            break;
          case STARTUPNTRIPSERV:
            str += " ntripservice";
            break;
          case STARTUPJT808SERV:
            str += " jt808service";
            break;
        }
        if (va_vec.empty()) {
          break;
        }
      }
    }
    str.copy(result, str.size(), 0);
  }

  return retval;
}

static int SearchStringFormList(const std::vector<std::string> &va_vec,
                                const std::string &arg) {
  auto va_it = va_vec.begin();
  while (va_it != va_vec.end()) {
    if (*va_it == arg) {
      break;
    }
    ++va_it;
  }
  return (va_it == va_vec.end() ? 0 : 1);
}

int Jt808Service::DealSetStartupRequest(DeviceNode &device,
                                        std::vector<std::string> &va_vec) {
  std::vector<std::string> va_vec_old(va_vec.begin(), va_vec.end());
  va_vec.clear();
  std::string arg = "F000:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "gps"));
  va_vec.push_back(arg);
  arg = "F001:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "cdradio"));
  va_vec.push_back(arg);
  arg = "F002:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "ntripcors"));
  va_vec.push_back(arg);
  arg = "F003:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "ntripservice"));
  va_vec.push_back(arg);
  arg = "F004:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "jt808service"));
  va_vec.push_back(arg);

  va_vec_old.clear();
  reverse(va_vec.begin(), va_vec.end());
  return DealSetTerminalParameterRequest(device, va_vec);
}

int Jt808Service::DealGetGpsRequest(DeviceNode &device, char *result) {
  int retval = -1;
  char parameter_value[2] = {0};
  uint32_t parameter_id;
  std::vector<std::string> va_vec;
  std::vector<uint32_t> id_vec;

  id_vec.push_back(GPSLOGGGA);
  id_vec.push_back(GPSLOGRMC);
  id_vec.push_back(GPSLOGATT);
  PrepareParemeterIdList(va_vec, id_vec);
  if ((retval = DealGetTerminalParameterRequest(device, va_vec)) == 0) {
    std::string arg;
    std::string str = "gps:";
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      sscanf(arg.c_str(), "%x:%s", &parameter_id, parameter_value);
      if (parameter_value[0] == '1') {
        switch (parameter_id) {
          case GPSLOGGGA:
            str += " LOGGGA";
            break;
          case GPSLOGRMC:
            str += " LOGRMC";
            break;
          case GPSLOGATT:
            str += " LOGATT";
            break;
          default:
            continue;
        }
      }
    }
    str.copy(result, str.size(), 0);
  }

  return retval;
}

int Jt808Service::DealSetGpsRequest(DeviceNode &device,
                                    std::vector<std::string> &va_vec) {
  std::vector<std::string> va_vec_old(va_vec.begin(), va_vec.end());
  va_vec.clear();
  std::string arg = "F010:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "LOGGGA"));
  va_vec.push_back(arg);
  arg = "F011:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "LOGRMC"));
  va_vec.push_back(arg);
  arg = "F012:";
  arg += std::to_string(SearchStringFormList(va_vec_old, "LOGATT"));
  va_vec.push_back(arg);

  va_vec_old.clear();
  reverse(va_vec.begin(), va_vec.end());
  return DealSetTerminalParameterRequest(device, va_vec);
}

int Jt808Service::DealGetCdradioRequest(DeviceNode &device, char *result) {
  int retval = -1;
  char parameter_value[256] = {0};
  uint32_t parameter_id;
  std::vector<std::string> va_vec;
  std::vector<uint32_t> id_vec;

  id_vec.push_back(CDRADIOBAUDERATE);
  id_vec.push_back(CDRADIOWORKINGFREQ);
  id_vec.push_back(CDRADIORECEIVEMODE);
  id_vec.push_back(CDRADIOFORMCODE);
  PrepareParemeterIdList(va_vec, id_vec);
  if ((retval = DealGetTerminalParameterRequest(device, va_vec)) == 0) {
    std::string arg;
    std::string str = "cdradio: ";
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      memset(parameter_value, 0x0, sizeof(parameter_value));
      sscanf(arg.c_str(), "%x:%s", &parameter_id, parameter_value);
      switch (parameter_id) {
        case CDRADIOBAUDERATE:
          str += "bauderate=";
          str += parameter_value;
          break;
        case CDRADIOWORKINGFREQ:
          str += "workfreqpoint=";
          str += parameter_value;
          break;
        case CDRADIORECEIVEMODE:
          str += "recvmode=";
          str += parameter_value;
          break;
        case CDRADIOFORMCODE:
          str += "formcode=";
          str += parameter_value;
          break;
        default:
          continue;
      }
      if (va_vec.empty()) {
        break;
      }
      str += ",";
    }
    str.copy(result, str.size(), 0);
  }

  return retval;
}

int Jt808Service::DealSetCdradioRequest(DeviceNode &device,
                                        std::vector<std::string> &va_vec) {
  char parameter_value[256] = {0};
  uint32_t parameter_id = CDRADIOBAUDERATE;

  if (va_vec.size() != 4) {
    return -1;
  }

  reverse(va_vec.begin(), va_vec.end());
  auto va_it = va_vec.begin();
  while (va_it != va_vec.end()) {
    memset(parameter_value, 0x0, sizeof(parameter_value));
    snprintf(parameter_value, sizeof(parameter_value), "%04X:%s",
             parameter_id, va_it->c_str());
    *va_it = parameter_value;
    ++parameter_id;
    ++va_it;
  }

  reverse(va_vec.begin(), va_vec.end());
  return DealSetTerminalParameterRequest(device, va_vec);
}

int Jt808Service::DealGetNtripCorsRequest(DeviceNode &device, char *result) {
  int retval = -1;
  char parameter_value[256] = {0};
  uint32_t parameter_id;
  std::vector<std::string> va_vec;
  std::vector<uint32_t> id_vec;

  id_vec.push_back(NTRIPCORSIP);
  id_vec.push_back(NTRIPCORSPORT);
  id_vec.push_back(NTRIPCORSUSERNAME);
  id_vec.push_back(NTRIPCORSPASSWD);
  id_vec.push_back(NTRIPCORSMOUNTPOINT);
  id_vec.push_back(NTRIPCORSREPORTINTERVAL);
  PrepareParemeterIdList(va_vec, id_vec);
  if ((retval = DealGetTerminalParameterRequest(device, va_vec)) == 0) {
    std::string arg;
    std::string str = "ntripcors: ";
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      memset(parameter_value, 0x0, sizeof(parameter_value));
      sscanf(arg.c_str(), "%x:%s", &parameter_id, parameter_value);
      switch (parameter_id) {
        case NTRIPCORSIP:
          str += "ip=";
          str += parameter_value;
          break;
        case NTRIPCORSPORT:
          str += "port=";
          str += parameter_value;
          break;
        case NTRIPCORSUSERNAME:
          str += "username=";
          str += parameter_value;
          break;
        case NTRIPCORSPASSWD:
          str += "password=";
          str += parameter_value;
          break;
        case NTRIPCORSMOUNTPOINT:
          str += "mountpoint=";
          str += parameter_value;
          break;
        case NTRIPCORSREPORTINTERVAL:
          str += "reportinterval=";
          str += parameter_value;
          break;
        default:
          continue;
      }
      if (va_vec.empty()) {
        break;
      }
      str += ",";
    }
    str.copy(result, str.size(), 0);
  }

  return retval;
}

int Jt808Service::DealSetNtripCorsRequest(DeviceNode &device,
                                          std::vector<std::string> &va_vec) {
  char parameter_value[256] = {0};
  uint32_t parameter_id = NTRIPCORSIP;

  if (va_vec.size() != 6) {
    return -1;
  }

  reverse(va_vec.begin(), va_vec.end());
  auto va_it = va_vec.begin();
  while (va_it != va_vec.end()) {
    memset(parameter_value, 0x0, sizeof(parameter_value));
    snprintf(parameter_value, sizeof(parameter_value), "%04X:%s",
             parameter_id, va_it->c_str());
    *va_it = parameter_value;
    ++parameter_id;
    ++va_it;
  }

  reverse(va_vec.begin(), va_vec.end());
  return DealSetTerminalParameterRequest(device, va_vec);
}

int Jt808Service::DealGetNtripServiceRequest(DeviceNode &device, char *result) {
  int retval = -1;
  char parameter_value[256] = {0};
  uint32_t parameter_id;
  std::vector<std::string> va_vec;
  std::vector<uint32_t> id_vec;

  id_vec.push_back(NTRIPSERVICEIP);
  id_vec.push_back(NTRIPSERVICEPORT);
  id_vec.push_back(NTRIPSERVICEUSERNAME);
  id_vec.push_back(NTRIPSERVICEPASSWD);
  id_vec.push_back(NTRIPSERVICEMOUNTPOINT);
  id_vec.push_back(NTRIPSERVICEREPORTINTERVAL);
  PrepareParemeterIdList(va_vec, id_vec);
  if ((retval = DealGetTerminalParameterRequest(device, va_vec)) == 0) {
    std::string arg;
    std::string str = "ntripservice: ";
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      memset(parameter_value, 0x0, sizeof(parameter_value));
      sscanf(arg.c_str(), "%x:%s", &parameter_id, parameter_value);
      switch (parameter_id) {
        case NTRIPSERVICEIP:
          str += "ip=";
          str += parameter_value;
          break;
        case NTRIPSERVICEPORT:
          str += "port=";
          str += parameter_value;
          break;
        case NTRIPSERVICEUSERNAME:
          str += "username=";
          str += parameter_value;
          break;
        case NTRIPSERVICEPASSWD:
          str += "password=";
          str += parameter_value;
          break;
        case NTRIPSERVICEMOUNTPOINT:
          str += "mountpoint=";
          str += parameter_value;
          break;
        case NTRIPSERVICEREPORTINTERVAL:
          str += "reportinterval=";
          str += parameter_value;
          break;
        default:
          continue;
      }
      if (va_vec.empty()) {
        break;
      }
      str += ",";
    }
    str.copy(result, str.size(), 0);
  }

  return retval;
}

int Jt808Service::DealSetNtripServiceRequest(DeviceNode &device,
                                             std::vector<std::string> &va_vec) {
  char parameter_value[256] = {0};
  uint32_t parameter_id = NTRIPSERVICEIP;

  if (va_vec.size() != 6) {
    return -1;
  }

  reverse(va_vec.begin(), va_vec.end());
  auto va_it = va_vec.begin();
  while (va_it != va_vec.end()) {
    memset(parameter_value, 0x0, sizeof(parameter_value));
    snprintf(parameter_value, sizeof(parameter_value), "%04X:%s",
             parameter_id, va_it->c_str());
    *va_it = parameter_value;
    ++parameter_id;
    ++va_it;
  }

  reverse(va_vec.begin(), va_vec.end());
  return DealSetTerminalParameterRequest(device, va_vec);
}

int Jt808Service::DealGetJt808ServiceRequest(DeviceNode &device, char *result) {
  int retval = -1;
  char parameter_value[256] = {0};
  uint32_t parameter_id;
  std::vector<std::string> va_vec;
  std::vector<uint32_t> id_vec;

  id_vec.push_back(JT808SERVICEIP);
  id_vec.push_back(JT808SERVICEPORT);
  id_vec.push_back(JT808SERVICEPHONENUM);
  id_vec.push_back(JT808SERVICEREPORTINTERVAL);
  PrepareParemeterIdList(va_vec, id_vec);
  if ((retval = DealGetTerminalParameterRequest(device, va_vec)) == 0) {
    std::string arg;
    std::string str = "jt808service: ";
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      memset(parameter_value, 0x0, sizeof(parameter_value));
      sscanf(arg.c_str(), "%x:%s", &parameter_id, parameter_value);
      switch (parameter_id) {
        case JT808SERVICEIP:
          str += "ip=";
          str += parameter_value;
          break;
        case JT808SERVICEPORT:
          str += "port=";
          str += parameter_value;
          break;
        case JT808SERVICEPHONENUM:
          str += "phonenum=";
          str += parameter_value;
          break;
        case JT808SERVICEREPORTINTERVAL:
          str += "reportinterval=";
          str += parameter_value;
          break;
        default:
          continue;
      }
      if (va_vec.empty()) {
        break;
      }
      str += ",";
    }
    str.copy(result, str.size(), 0);
  }

  return retval;
}

int Jt808Service::DealSetJt808ServiceRequest(DeviceNode &device,
                                             std::vector<std::string> &va_vec) {
  char parameter_value[256] = {0};
  uint32_t parameter_id = JT808SERVICEIP;

  if (va_vec.size() != 4) {
    return -1;
  }

  reverse(va_vec.begin(), va_vec.end());
  auto va_it = va_vec.begin();
  while (va_it != va_vec.end()) {
    memset(parameter_value, 0x0, sizeof(parameter_value));
    snprintf(parameter_value, sizeof(parameter_value), "%04X:%s",
             parameter_id, va_it->c_str());
    *va_it = parameter_value;
    ++parameter_id;
    ++va_it;
  }

  reverse(va_vec.begin(), va_vec.end());
  return DealSetTerminalParameterRequest(device, va_vec);
}

int Jt808Service::DealGetTerminalParameterRequest(
                      DeviceNode &device, std::vector<std::string> &va_vec) {
  int retval = -1;
  uint16_t u16val;
  uint32_t u32val;
  uint32_t parameter_id;
  std::string arg;
  ProtocolParameters propara = {0};
  MessageData msg = {0};

  PreparePhoneNum(device.phone_num, propara.phone_num);
  propara.terminal_parameter_id_count = 0;
  if (va_vec.empty()) {
    Jt808FramePack(msg, DOWN_GETTERMPARA, propara);
  } else {
    propara.terminal_parameter_id_buffer = new uint8_t [va_vec.size() * 4];
    uint8_t *ptr = propara.terminal_parameter_id_buffer;
    while (!va_vec.empty()) {
      arg = va_vec.back();
      va_vec.pop_back();
      sscanf(arg.c_str(), "%X", &u32val);
      parameter_id = EndianSwap32(u32val);
      memcpy(ptr, &parameter_id, 4);
      ptr += 4;
      propara.terminal_parameter_id_count++;
    }
    Jt808FramePack(msg, DOWN_GETSPECTERMPARA, propara);
    delete [] propara.terminal_parameter_id_buffer;
  }

  if (SendFrameData(device.socket_fd, msg)) {
    close(device.socket_fd);
    device.socket_fd = -1;
  } else {
    propara.terminal_parameter_list = new std::list<TerminalParameter *>;
    while (1) {
      memset(&msg, 0x0, sizeof(msg));
      if (RecvFrameData(device.socket_fd, msg)) {
        close(device.socket_fd);
        device.socket_fd = -1;
        break;
      } else if (msg.len > 0) {
        if (Jt808FrameParse(msg, propara) == UP_GETPARASPONSE) {
          memset(&msg, 0x0, sizeof(msg));
          Jt808FramePack(msg, DOWN_UNIRESPONSE, propara);
          if (SendFrameData(device.socket_fd, msg)) {
            close(device.socket_fd);
            device.socket_fd = -1;
            break;
          }
          if (propara.packet_total_num != propara.packet_sequence_num) {
            continue;
          }
          char parameter_s[512] = {0};
          auto para_it = propara.terminal_parameter_list->begin();
          while (para_it != propara.terminal_parameter_list->end()) {
            memset(parameter_s, 0x0, sizeof(parameter_s));
            u16val = GetParameterTypeByParameterId((*para_it)->parameter_id);
            if (u16val == kStringType) {
              snprintf(parameter_s, sizeof(parameter_s), "%04X:%s",
                       (*para_it)->parameter_id, (*para_it)->parameter_value);
            } else {
              u32val = 0;
              memcpy(&u32val, (*para_it)->parameter_value,
                     (*para_it)->parameter_len);
              snprintf(parameter_s, sizeof(parameter_s), "%04X:%u",
                       (*para_it)->parameter_id, u32val);
            }
            arg = parameter_s;
            va_vec.push_back(arg);
            ++para_it;
          }
          reverse(va_vec.begin(), va_vec.end());
          retval = 0;
          break;
        }
      }
    }
  }
  ClearListElement(propara.terminal_parameter_list);
  return retval;
}

int Jt808Service::DealSetTerminalParameterRequest(
                      DeviceNode &device, std::vector<std::string> &va_vec) {
  int retval = 0;
  char parameter_value[256] = {0};
  char value[256] = {0};
  uint8_t u8val = 0;
  uint16_t u16val = 0;
  uint16_t data_len = 0;
  uint32_t u32val = 0;
  uint32_t parameter_id = 0;
  std::string arg;
  ProtocolParameters propara = {0};
  MessageData msg = {0};

  auto va_it = va_vec.begin();
  while (va_it != va_vec.end()) {
    data_len += 5 + va_it->size();
    ++va_it;
  }

  if (data_len > 1022) {
    propara.packet_total_num = data_len/1022 + 1;
    propara.packet_sequence_num = 1;
  }

  while (1) {
    propara.terminal_parameter_list = new std::list<TerminalParameter *>;
    data_len = 0;
    while (!va_vec.empty()) {
      arg = va_vec.back();
      memset(value, 0x0, sizeof(value));
      sscanf(arg.c_str(), "%x:%s", &u32val, value);
      parameter_id = u32val;
      memset(parameter_value, 0x0, sizeof(parameter_value));
      switch (GetParameterTypeByParameterId(parameter_id)) {
        case kByteType:
          u8val = atoi(value);
          memcpy(parameter_value, &u8val, 1);
          break;
        case kWordType:
          u16val = atoi(value);
          memcpy(parameter_value, &u16val, 2);
          break;
        case kDwordType:
          u32val = atoi(value);
          memcpy(parameter_value, &u32val, 4);
          break;
        case kStringType:
          memcpy(parameter_value, value, strlen(value));
          break;
        case kUnknowType:
          continue;
      }
      if ((data_len + 5 + strlen(value))> 1022) {
        break;
      }
      data_len += 5 + strlen(value);
      va_vec.pop_back();
      AddParameterNodeIntoList(propara.terminal_parameter_list,
                               parameter_id, parameter_value);
    }

    if (propara.terminal_parameter_list->empty()) {
      return retval;
    }

    PreparePhoneNum(device.phone_num, propara.phone_num);
    Jt808FramePack(msg, DOWN_SETTERMPARA, propara);
    SendFrameData(device.socket_fd, msg);
    while (1) {
      memset(&msg, 0x0, sizeof(msg));
      if (RecvFrameData(device.socket_fd, msg)) {
        close(device.socket_fd);
        device.socket_fd = -1;
        break;
      } else if (msg.len > 0) {
        if (Jt808FrameParse(msg, propara) &&
            (propara.respond_id == DOWN_SETTERMPARA)) {
          break;
        }
      }
    }
    ClearListElement(propara.terminal_parameter_list);
    if (va_vec.empty()) {
      break;
    }
    if (propara.packet_total_num > propara.packet_sequence_num) {
      ++propara.packet_sequence_num;
    }
  }
  return retval;
}

int Jt808Service::ParseCommand(char *buffer) {
  int retval = 0;
  std::string arg;
  std::string phone_num;
  std::stringstream sstr;
  std::vector<std::string> va_vec;

  sstr.clear();
  sstr << buffer;
  do {
    arg.clear();
    sstr >> arg;
    if (arg.empty()) {
      break;
    }
    va_vec.push_back(arg);
  } while (1);
  sstr.str("");
  sstr.clear();
  memset(buffer, 0x0, strlen(buffer));
  reverse(va_vec.begin(), va_vec.end());
  // auto va_it = va_vec.begin();
  // while (va_it != va_vec.end()) {
  //   printf("%s\n", va_it->c_str());
  //   va_it;
  // }

  arg = va_vec.back();
  va_vec.pop_back();
  if (!device_list_.empty()) {
    auto device_it = device_list_.begin();
    while (device_it != device_list_.end()) {
      phone_num = device_it->phone_num;
      if (arg == phone_num) {
        break;
      }
      ++device_it;
    }

    if (device_it != device_list_.end() && (device_it->socket_fd > 0)) {
      arg = va_vec.back();
      va_vec.pop_back();
      if (arg == "upgrade") {
        arg = va_vec.back();
        va_vec.pop_back();
        if ((arg == "device") || (arg == "gps") ||
            (arg == "system") || (arg == "cdradio")) {
          if (arg == "device") {
            device_it->upgrade_type = 0x0;
          } else if (arg == "gps") {
            device_it->upgrade_type = 0x34;
          } else if (arg == "cdradio") {
            device_it->upgrade_type = 0x35;
          } else if (arg == "system") {
            device_it->upgrade_type = 0x36;
          } else {
            return -1;
          }

          arg = va_vec.back();
          va_vec.pop_back();
          memset(device_it->upgrade_version,
                 0x0, sizeof(device_it->upgrade_version));
          arg.copy(device_it->upgrade_version, arg.length(), 0);
          arg = va_vec.back();
          va_vec.pop_back();
          memset(device_it->file_path, 0x0, sizeof(device_it->file_path));
          arg.copy(device_it->file_path, arg.length(), 0);
          device_it->has_upgrade = true;
          // start upgrade deal thread.
          std::thread start_upgrade_thread(StartUpgradeThread, this);
          start_upgrade_thread.detach();
          memcpy(buffer, "operation completed.", 20);
        }
      } else if (arg == "get") {
        EpollUnregister(epoll_fd_, device_it->socket_fd);
        arg = va_vec.back();
        va_vec.pop_back();
        if (arg == "startup") {
          retval = DealGetStartupRequest(*device_it, buffer);
        } else if (arg == "gps") {
          retval = DealGetGpsRequest(*device_it, buffer);
        } else if (arg == "cdradio") {
          retval = DealGetCdradioRequest(*device_it, buffer);
        } else if (arg == "ntripcors") {
          retval = DealGetNtripCorsRequest(*device_it, buffer);
        } else if (arg == "ntripservice") {
          retval = DealGetNtripServiceRequest(*device_it, buffer);
        } else if (arg == "jt808service") {
          retval = DealGetJt808ServiceRequest(*device_it, buffer);
        } else if (arg == "terminalparameter") {
          retval = DealGetTerminalParameterRequest(*device_it, va_vec);
          if (retval == 0) {
            std::string result = "terminal parameter(id:value): ";
            while (!va_vec.empty()) {
              result += va_vec.back();
              va_vec.pop_back();
              if (va_vec.empty()) {
                break;
              }
              result += ",";
            }
            result.copy(buffer, result.size(), 0);
          }
        }
        EpollRegister(epoll_fd_, device_it->socket_fd);
      } else if (arg == "set") {
        EpollUnregister(epoll_fd_, device_it->socket_fd);
        arg = va_vec.back();
        va_vec.pop_back();
        if (arg == "startup") {
          retval = DealSetStartupRequest(*device_it, va_vec);
        } else if (arg == "gps") {
          retval = DealSetGpsRequest(*device_it, va_vec);
        } else if (arg == "cdradio") {
          retval = DealSetCdradioRequest(*device_it, va_vec);
        } else if (arg == "ntripcors") {
          retval = DealSetNtripCorsRequest(*device_it, va_vec);
        } else if (arg == "ntripservice") {
          retval = DealSetNtripServiceRequest(*device_it, va_vec);
        } else if (arg == "jt808service") {
          retval = DealSetJt808ServiceRequest(*device_it, va_vec);
        } else if (arg == "terminalparameter") {
          retval = DealSetTerminalParameterRequest(*device_it, va_vec);
        }
        if (retval == 0) {
          memcpy(buffer, "operation completed.", 20);
        } else {
          memcpy(buffer, "operation failed!!!", 19);
        }
        EpollRegister(epoll_fd_, device_it->socket_fd);
      }
    } else if (device_it != device_list_.end()) {
      memcpy(buffer, "device has not connect!!!\n", 25);
    } else {
      memcpy(buffer, "has not such device!!!\n", 22);
    }
  }

  va_vec.clear();
  return retval;
}

void Jt808Service::UpgradeHandler(void) {
  MessageData msg;
  ProtocolParameters propara;
  std::ifstream ifs;
  char *data = nullptr;
  uint32_t len;
  uint32_t max_data_len;

  if (!device_list_.empty()) {
    auto device_it = device_list_.begin();
    while (device_it != device_list_.end()) {
      if (device_it->has_upgrade) {
        device_it->has_upgrade = false;
        break;
      }
      ++device_it;
    }

    if (device_it == device_list_.end()) {
      return ;
    }

    memset(&propara, 0x0, sizeof(propara));
    memcpy(propara.version_num, device_it->upgrade_version,
           strlen(device_it->upgrade_version));
    max_data_len = 1023 - 11 - strlen(device_it->upgrade_version);
    EpollUnregister(epoll_fd_, device_it->socket_fd);
    ifs.open(device_it->file_path, std::ios::binary | std::ios::in);
    if (ifs.is_open()) {
      ifs.seekg(0, std::ios::end);
      len = ifs.tellg();
      ifs.seekg(0, std::ios::beg);
      data = new char[len];
      ifs.read(data, len);
      ifs.close();

      propara.packet_total_num = len/max_data_len + 1;
      propara.packet_sequence_num = 1;
      propara.upgrade_type = device_it->upgrade_type;
      propara.version_num_len = strlen(device_it->upgrade_version);
      while (len > 0) {
        memset(msg.buffer, 0x0, MAX_PROFRAMEBUF_LEN);
        if (len > max_data_len) {
          propara.packet_data_len = max_data_len;
        } else {
          propara.packet_data_len = len;
        }
        len -= propara.packet_data_len;
        // prepare data content of the upgrade file.
        memset(propara.packet_data, 0x0, sizeof(propara.packet_data));
        memcpy(propara.packet_data,
               data + max_data_len * (propara.packet_sequence_num - 1),
               propara.packet_data_len);
        msg.len = Jt808FramePack(msg, DOWN_UPDATEPACKAGE, propara);
        if (SendFrameData(device_it->socket_fd, msg)) {
          close(device_it->socket_fd);
          device_it->socket_fd = -1;
          break;
        } else {
          while (1) {
            if (RecvFrameData(device_it->socket_fd, msg)) {
              close(device_it->socket_fd);
              device_it->socket_fd = -1;
              break;
            } else if (msg.len > 0) {
              if ((Jt808FrameParse(msg, propara) == UP_UNIRESPONSE) &&
                  (propara.respond_id == DOWN_UPDATEPACKAGE)) {
                break;
              }
            }
          }
          if (device_it->socket_fd == -1) {
            break;
          }
          ++propara.packet_sequence_num;
          usleep(1000);
        }
      }

      if (device_it->socket_fd > 0) {
        EpollRegister(epoll_fd_, device_it->socket_fd);
      }
      delete [] data;
      data = nullptr;
    }
  }
}

