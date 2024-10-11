/*--------------------------------------------------------------------
  (C) Copyright 2017-2020 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion

  This file is part of OmpSs@FPGA toolchain.

  This code is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 3 of
  the License, or (at your option) any later version.

  OmpSs@FPGA toolchain is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this code. If not, see <www.gnu.org/licenses/>.
--------------------------------------------------------------------*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include "../util/ticket-lock.h"

#include "../libxdma.h"

#define QDMA_Q_IDX       1
#define QDMA_DEV_ID_ENV  "XDMA_QDMA_DEV"

#define DEV_ALIGN         (512/8) //Buses are 512b wide
#define DEV_MEM_SIZE      0x800000000 ///<Device memory (32GB)
#define DEV_MEM_SIZE_ENV  "XDMA_DEV_MEM_SIZE"

// With big transfers, the network doesn't send everything in a single call
#define MAX_NETWORK_TRANSFER_SIZE 536870912 //512MB
//For some reason, QDMA 5 from vivado 2022.2 fails when using larger chunks
#define MAX_TRANSFER_SIZE 1*1024*1024
#define MAX_DEVICES 8
#define MAX_NODES 16
#define MAX_CLUSTER (MAX_DEVICES*MAX_NODES)

static int _sockfd[MAX_NODES];
static int _ndevs = 0;
static int _cluster_size = 0;
static int _nnodes = 0;
static int _client_node = 0;
static int _qdmaFd[MAX_DEVICES];
static int _nodeid;
static int _fpgaid2localid[MAX_CLUSTER];
static int _fpgaid2nodeid[MAX_CLUSTER];
static int _nodeid2port[MAX_NODES];
static struct in_addr _nodeid2ip[MAX_NODES];

static uintptr_t _curDevMemPtr[MAX_CLUSTER];

static ticketLock_t _copyMutexD[MAX_DEVICES];
static ticketLock_t _copyMutexN[MAX_NODES];

// Internal library representation of an alloc
typedef struct {
    int devId;
    uint64_t devPtr;
} alloc_info_t;


// Get dev mem size from env variable or use the default
static size_t getDeviceMemSize(){
    const char* devMemSize = getenv(DEV_MEM_SIZE_ENV);
    if (!devMemSize)
        return DEV_MEM_SIZE;
    else
        return (size_t) strtoull(devMemSize, NULL, 10);
}

// Get qdma device id from env variable
static const char *getDeviceList() {
    const char* devIdList = getenv(QDMA_DEV_ID_ENV);
    if (devIdList == NULL) {
        fprintf(stderr, "[XDMA] Environment variable " QDMA_DEV_ID_ENV " not set, it should contain the QDMA device ID list\n");
    }
    return devIdList;
}

static int read_cluster_config() {
    const char *filename = getenv("XTASKS_CLUSTER_FILE");
    if (filename == NULL) {
        filename = "xtasks.cluster";
    }

    FILE* file = fopen(filename, "r");
    if (file == NULL) { // Assuming we are in single-node mode
        _nnodes = 0;
        _cluster_size = 0;
        _nodeid = 0;
        _client_node = 0;
        return 0;
    }

    int r;
    r = fscanf(file, "%d %d %d", &_cluster_size, &_nnodes, &_client_node);
    if (r != 3) {
        fprintf(stderr, "xtasks.cluster format not recognized\n");
        fclose(file);
        return 1;
    }
    for (int i = 0; i < _cluster_size; ++i) {
        r = fscanf(file, "%d %d", &_fpgaid2localid[i], &_fpgaid2nodeid[i]);
        if (r != 2) {
            fprintf(stderr, "xtasks.cluster format not recognized\n");
            fclose(file);
            return 1;
        }
    }
    char myhostname[32];
    if (gethostname(myhostname, 31)) {
        perror("gethostname with 31 len errno");
        return 1;
    }
    _nodeid = -1;
    for (int i = 0; i < _nnodes; ++i) {
        char ip[16]; // Longest IP string is 15 char (255.255.255.255)
        char fhostname[32];
        r = fscanf(file, "%15s %d %31s", ip, &_nodeid2port[i], fhostname);
        if (r != 3) {
            fprintf(stderr, "xtasks.cluster format not recognized\n");
            fclose(file);
            return 1;
        }
        if (inet_pton(AF_INET, ip, &_nodeid2ip[i]) != 1) {
            fprintf(stderr, "Invalid IP address %s\n", ip);
            fclose(file);
            return 1;
        }
        if (strcmp(fhostname, myhostname) == 0) {
            if (_nodeid == -1) {
                _nodeid = i;
            }
            else {
                fprintf(stderr, "Found duplicated hostname %s in xtasks.cluster\n", fhostname);
                fclose(file);
                return 1;
            }
        }
    }
    fclose(file);
    if (_nodeid == -1) {
        fprintf(stderr, "Could not find hostname %s in xtasks.cluster\n", myhostname);
        return 1;
    }

    return 0;
}

static int init_sockets() {
    struct sockaddr_in servaddr;

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;

    int curnode = 0;
    for (int n = 0; n < _nnodes; ++n) {
        if (n == _nodeid)
            continue;

        _sockfd[n] = socket(AF_INET, SOCK_STREAM, 0);
        if (_sockfd[n] < 0) {
            perror("Could not open socket");
            goto err;
        }
        ++curnode;

        servaddr.sin_addr = _nodeid2ip[n];
        servaddr.sin_port = htons(_nodeid2port[n]);

        if (connect(_sockfd[n], (const struct sockaddr*) &servaddr, sizeof(servaddr)) != 0) {
            perror("Error connecting");
            goto err;
        }
    }

    return 0;

    err:
    for (int n = 0; n < curnode; ++n) {
        if (n != _nodeid)
            close(_sockfd[n]);
    }
    return 1;
}

xdma_status xdmaInit() {
    const char* devListEnv = getDeviceList();
    if (devListEnv == NULL) {
        return XDMA_ERROR;
    }
    if (read_cluster_config() != 0) {
        return XDMA_ERROR;
    }
    // Init sockets only if I'm the client node
    if (_nodeid == _client_node && init_sockets() != 0) {
        return XDMA_ERROR;
    }
    int ndevs = 0;
    _ndevs = 0;
    char *devId;
    char *devList = malloc(strlen(devListEnv)+1);
    strcpy(devList, devListEnv);
    devId = strtok(devList, " ");
    while (devId != NULL) {
        if (ndevs == MAX_DEVICES) {
            fprintf(stderr, "Found too many devices\n");
            goto init_maxdev_err;
        }

        char devFileName[24];
        sprintf(devFileName, "/dev/qdma%s-MM-%d", devId, QDMA_Q_IDX);

        _qdmaFd[ndevs] = open(devFileName, O_RDWR);
        if (_qdmaFd[ndevs] < 0) {
            perror("XDMA open error");
            if (errno == ENOENT) {
                fprintf(stderr, "%s not found!\n", devFileName);
            }
            goto init_open_err;
        }

        fprintf(stderr, "[XDMA] Found device %s\n", devId);

        ++ndevs;
        devId = strtok(NULL, " ");
    }
    free(devList);

    for (int i = 0; i < ndevs; ++i) {
        //Initialize dummy allocator
        ticketLockInit(&_copyMutexD[i]);
        if (_nnodes == 0) _curDevMemPtr[i] = 0;
    }
    for (int i = 0; i < _nnodes; ++i) ticketLockInit(&_copyMutexN[i]);
    for (int i = 0; i < _cluster_size; ++i) _curDevMemPtr[i] = 0;
    _ndevs = ndevs;

    return XDMA_SUCCESS;

init_maxdev_err:
init_open_err:
    for (int d = 0; d < ndevs; ++d)
        close(_qdmaFd[d]);
    if (_nodeid == _client_node)
        for (int n = 0; n < _nnodes; ++n)
            if (n != _nodeid)
                close(_sockfd[n]);
    free(devList);

    return XDMA_ERROR;
}

xdma_status xdmaFini() {
    //close queue files
    //stop queues
    //delete queues
    for (int i = 0; i < _ndevs; ++i) {
        if (_qdmaFd[i] > 0) {
            close(_qdmaFd[i]);
        }
    }

    if (_nodeid == _client_node)
        for (int n = 0; n < _nnodes; ++n) {
            if (n != _nodeid)
                close(_sockfd[n]);
        }

    return XDMA_SUCCESS;
}

xdma_status xdmaGetNumDevices(int *numDevices) {
    if (_nnodes == 0) {
        *numDevices = _ndevs;
    }
    else {
        *numDevices = _cluster_size;
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaAllocateHost(int devId, void **buffer, xdma_buf_handle *handle, size_t len) {
    //QDMA does not support memory mapped device buffers
    return XDMA_ENOSYS;
}

xdma_status xdmaAllocate(int devId, xdma_buf_handle *handle, size_t len) {
    uint64_t nlen = ((len + (DEV_ALIGN + 1))/DEV_ALIGN)*DEV_ALIGN;
    uint64_t ptr = __atomic_fetch_add(_curDevMemPtr + devId, nlen, __ATOMIC_RELAXED);
    //adjust size so we always get aligned addresses
    if (ptr + nlen > getDeviceMemSize()) {  //_curDevMemPtr starts at 0
        return XDMA_ENOMEM;
    }

    alloc_info_t* alloc_info = (alloc_info_t*)malloc(sizeof(alloc_info_t));
    alloc_info->devPtr = ptr;
    alloc_info->devId = devId;

    *handle = alloc_info;
    return XDMA_SUCCESS;
}

xdma_status xdmaFree(xdma_buf_handle handle) {
    free((alloc_info_t*)handle);
    return XDMA_SUCCESS;
}

static inline size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

xdma_status xdmaMemcpy(void *usr, xdma_buf_handle handle, size_t len, size_t offset,
        xdma_dir mode) {
    ssize_t tx;
    size_t transferred = 0, rem = len;
    alloc_info_t* alloc_info = (alloc_info_t*)handle;

    int devId = alloc_info->devId;
    int localId;
    int nodeId;
    if (_nnodes != 0) {
        localId = _fpgaid2localid[devId];
        nodeId = _fpgaid2nodeid[devId];
    } else {
        localId = devId;
        nodeId = _nodeid;
    }

    if (nodeId != _nodeid) {
        ticketLockAcquire(&_copyMutexN[nodeId]);
        int sockfd = _sockfd[nodeId];
        ssize_t n;
        uint32_t header = 0; //XDMA_MEMCPY
        alloc_info_t local_alloc_info;
        uint32_t ret = XDMA_ERROR;

        local_alloc_info.devId = devId;
        local_alloc_info.devPtr = alloc_info->devPtr;

        n = send(sockfd, &header, sizeof(header), MSG_MORE);
        if (n < 0) {
            perror("Error in send");
            goto socket_error;
        }
        else if (n != sizeof(header)) {
            fprintf(stderr, "Expected to send %lu bytes but found %ld\n", sizeof(header), n);
            goto socket_error;
        }

        uint64_t data[4];
        data[0] = sizeof(local_alloc_info);
        data[1] = offset;
        data[2] = len;
        data[3] = mode;

        n = send(sockfd, data, sizeof(data), MSG_MORE);
        if (n < 0) {
            perror("Error in send");
            goto socket_error;
        }
        else if (n != sizeof(data)) {
            fprintf(stderr, "Expected to send %lu bytes but found %ld\n", sizeof(data), n);
            goto socket_error;
        }

        n = send(sockfd, &local_alloc_info, sizeof(local_alloc_info), 0);
        if (n < 0) {
            perror("Error in send");
            goto socket_error;
        }
        else if (n != sizeof(local_alloc_info)) {
            fprintf(stderr, "Expected to send %lu bytes but found %ld\n", sizeof(local_alloc_info), n);
            goto socket_error;
        }

        if (mode == XDMA_TO_DEVICE) {
            for (uint64_t i = 0; i < len; i += MAX_NETWORK_TRANSFER_SIZE) {
                size_t t = min(MAX_NETWORK_TRANSFER_SIZE, len-i);
                n = send(sockfd, (const void*)((uintptr_t)usr + i), t, 0);
                if (n < 0) {
                    perror("Error in send");
                    goto socket_error;
                }
                else if ((size_t)n != t) {
                    fprintf(stderr, "Expected to send %lu bytes but found %ld\n", t, n);
                    goto socket_error;
                }
            }

            n = recv(sockfd, &ret, sizeof(ret), MSG_WAITALL);
            if (n < 0) {
                perror("Error in recv");
                ret = XDMA_ERROR;
                goto socket_error;
            }
            else if (n != sizeof(ret)) {
                fprintf(stderr, "Expected to receive %lu bytes but found %ld\n", sizeof(ret), n);
                ret = XDMA_ERROR;
                goto socket_error;
            }
        }
        else {
            for (uint64_t i = 0; i < len; i += MAX_NETWORK_TRANSFER_SIZE) {
                size_t t = min(MAX_NETWORK_TRANSFER_SIZE, len-i);
                n = recv(sockfd, (void*)((uintptr_t)usr + i), t, MSG_WAITALL);
                if (n < 0) {
                    perror("Error in recv");
                    goto socket_error;
                }
                else if ((size_t)n != t) {
                    fprintf(stderr, "Expected to receive %lu bytes but found %ld\n", t, n);
                    goto socket_error;
                }
            }
            ret = XDMA_SUCCESS;
        }

    socket_error:
        ticketLockRelease(&_copyMutexN[nodeId]);
        return ret;
    }

    uint64_t buffer = alloc_info->devPtr;

    off_t seekOff;
    off_t devOffset = (off_t)buffer + offset;
    ticketLockAcquire(&_copyMutexD[localId]);
    seekOff = lseek(_qdmaFd[localId], devOffset, SEEK_SET);
    if (seekOff != devOffset) {
        if (seekOff < 0) perror("XDMA dev offset:");
        ticketLockRelease(&_copyMutexD[localId]);
        return XDMA_ERROR;
    }
    if (mode == XDMA_TO_DEVICE) {
        while (transferred < len) {
            int chunkSize = rem < MAX_TRANSFER_SIZE ? rem : MAX_TRANSFER_SIZE;
            lseek(_qdmaFd[localId], devOffset + transferred, SEEK_SET);
            tx = write(_qdmaFd[localId], usr + transferred, chunkSize);
            rem -= tx;
            transferred += tx;
            if (tx < chunkSize) {
                perror("XDMA memcpy chunk error (trying to continue)");
            }
        }
    } else if (mode == XDMA_FROM_DEVICE) {
        while (transferred < len) {
            int chunkSize = rem < MAX_TRANSFER_SIZE ? rem : MAX_TRANSFER_SIZE;
            lseek(_qdmaFd[localId], devOffset + transferred, SEEK_SET);
            tx = read(_qdmaFd[localId], usr + transferred, chunkSize);
            rem -= tx;
            transferred += tx;
            if (tx < chunkSize) {
                perror("XDMA memcpy chunk error (trying to continue)");
            }
        }
    } else {
        ticketLockRelease(&_copyMutexD[localId]);
        return XDMA_ENOSYS; //Device to device transfers not yet implemented
    }
    ticketLockRelease(&_copyMutexD[localId]);
    if (transferred != len) {
        perror("XDMA memcpy error");
        return XDMA_ERROR;
    } else {
        return XDMA_SUCCESS;
    }
}

xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_dir mode, xdma_transfer_handle *transfer) {
    *transfer = 0;
    return xdmaMemcpy(usr, buffer, len, offset, mode);
}

xdma_status xdmaTestTransfer(xdma_transfer_handle *transfer) {
    return XDMA_SUCCESS;
}

xdma_status xdmaWaitTransfer(xdma_transfer_handle *transfer) {
    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceAddress(xdma_buf_handle handle, unsigned long *devAddress) {
    *devAddress = (unsigned long)((alloc_info_t*)handle)->devPtr;
    return XDMA_SUCCESS;
}

xdma_status xdmaInitHWInstrumentation() {
    return XDMA_ENOSYS;
}

xdma_status xdmaFiniHWInstrumentation() {
    return XDMA_ENOSYS;
}

xdma_status xdmaGetDeviceTime(int devId, uint64_t *time) {
    return XDMA_ENOSYS;
}

int xdmaInstrumentationEnabled() {
    return 0;
}

uint64_t xdmaGetInstrumentationTimerAddr(int devId) {
    return XDMA_ENOSYS;
}
