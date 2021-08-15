//
// Created by davej on 4/17/2020.
//
#include <vector>
#include <queue>
#include <cstring>
#include <iostream>
#include <fcntl.h>
#include <algorithm>
#include "VirtualMachine.h"
#include "Machine.h"

extern "C" {
TVMMainEntry VMLoadModule(const char *module);
void VMUnloadModule(void);

std::string path;


volatile int tickGl;
volatile int tickCnt = 0;
int FATfd;
TVMMutexID FATmtx;

std::vector<int> fds;
std::vector<int> dds;

void* memLoc;

struct TCB {
    TVMThreadID tID;
    TVMThreadEntry tEntry;
    void* stackBase;
    TVMMemorySize tMemorySize;
    TVMStatus tStatus;
    TVMThreadPriority tPriority;
    int result;
    int sleepLen;
    SMachineContext context;
    void* param;

    *TCB(TVMThreadID tID, TVMThreadEntry tEntry,void* stackBase, TVMMemorySize tMemorySize,TVMStatus tStatus,
         TVMThreadPriority tPriority,int result,int sleepLen, void* param)
            :tID(tID), tEntry(tEntry),stackBase(stackBase),tMemorySize(tMemorySize),tStatus(tStatus),
             tPriority(tPriority),result(result), sleepLen(sleepLen), param(param)
    {}
};

struct TCB* curThread;

std::vector<std::queue<struct TCB*>> pQ(4);
std::vector<struct TCB*> allThreads;
std::vector<struct TCB*> sleepers;

struct sharedMem {
    std::vector<void*> frags;
    volatile int used = 0;
};

struct sharedMem globalMem;

struct Mutex {
    TCB* owner;
    TVMMutexID mID;
    std::vector<std::queue<struct TCB*>> wQ;

    *Mutex(TCB* owner,TVMMutexID mID)
            :   owner(owner), mID(mID), wQ(std::vector<std::queue<struct TCB*>>(4))
    {}
};

std::vector<Mutex*> mtx;

#pragma pack(1)
struct BPB {
    uint8_t BS_jmpBoot[3];
    uint8_t BS_OEMName[8];
    uint16_t BPB_BytsPerSec;
    uint8_t BPB_SecPerClus;
    uint16_t BPB_RsvdSecCnt;
    uint8_t BPB_NumFATs;
    uint16_t BPB_RootEntCnt;
    uint16_t BPB_TotSec16;
    uint8_t BPB_Media;
    uint16_t BPB_FATSz16;
    uint16_t BPB_SecPerTrk;
    uint16_t BPB_NumHeads;
    uint32_t BPB_HiddSec;
    uint32_t BPB_TotSec32;
    uint8_t BS_DrvNum;
    uint8_t BS_Reserved1;
    uint8_t BS_BootSig;
    uint32_t BS_VolID;
    uint8_t BS_VolLab[11];
    uint8_t BS_FilSysType[8];

    unsigned int firstRootSector;// = BPB_RsvdSecCnt + BPB_NumFATs * BPB_FATSz16;
    unsigned int rootDirectorySectors;//  = (BPB_RootEntCnt * 32) / 512;
    unsigned int firstDataSector;//  = firstRootSector + rootDirectorySectors;
    unsigned int clusterCount;//  = ((long)BPB_TotSec32 - firstDataSector) / (int)BPB_SecPerClus;

};
#pragma pack()

struct BPB bpb;

std::vector<uint16_t> FAT;

#pragma pack(1)
struct RootEntry {
    uint8_t DIR_Name[11];
    uint8_t DIR_Attr;
    uint8_t DIR_NTRes;
    uint8_t DIR_CrtTimeTenth;
    uint16_t DIR_CrtTime;
    uint16_t DIR_CrtDate;
    uint16_t DIR_LstAccDate;
    uint16_t DIR_FstClusHI;
    uint16_t DIR_WrtTime;
    uint16_t DIR_WrtDate;
    uint16_t DIR_FstClusLo;
    uint32_t DIR_FileSize;
};
#pragma pack()

std::vector<struct RootEntry> Root;

struct Cluster {
    int clusNo;
    bool dirty = false;
    std::vector<uint8_t> clusData;

    explicit Cluster(int clNo)
            : clusNo(clNo), clusData(std::vector<uint8_t>(512*bpb.BPB_SecPerClus))
    {
    }
};

struct FileEnt {
    RootEntry* fileInRoot;
    SVMDirectoryEntry fDir;
    int firstClus;
    int filePos = 0;// filePos%clusterSize
    std::vector<struct Cluster> clusters;
};

std::vector<struct FileEnt> Files;
std::vector<struct FileEnt> Dirs;

void skelCreate(void* par) {
    MachineEnableSignals();
    struct TCB* p = (TCB*) par;
    p->tEntry(p->param);
    VMThreadTerminate(p->tID);
}

void idle(void* param) {
    MachineEnableSignals();
    while(1);
}

void schedule() {
    struct TCB* next;

    if(!pQ.at(3).empty()) {
        next = pQ.at(3).front();
        pQ.at(3).pop();
    } else if(!pQ.at(2).empty()) {
        next = pQ.at(2).front();
        pQ.at(2).pop();
    } else if(!pQ.at(1).empty()) {
        next = pQ.at(1).front();
        pQ.at(1).pop();
    } else {
        next = allThreads.at(1);
    }
    if(curThread->tStatus == VM_THREAD_STATE_RUNNING) {
        curThread->tStatus = VM_THREAD_STATE_READY;
        pQ.at(curThread->tPriority).push(curThread);
    }
    next->tStatus = VM_THREAD_STATE_RUNNING;
    MachineContextSave(&curThread->context);
    SMachineContextRef old = &curThread->context;
    curThread = next;
    MachineContextSwitch(old, &curThread->context);
}

void MRACallback(void *calldata) {
    tickCnt++;
    for(int i = 0; (unsigned)i < sleepers.size(); i++) {
        sleepers.at(i)->sleepLen--;
        if(sleepers.at(i)->sleepLen == 0) {
            sleepers.at(i)->tStatus = VM_THREAD_STATE_READY;
            pQ.at(sleepers.at(i)->tPriority).push(sleepers.at(i));
            sleepers.erase(sleepers.begin() + i);
        }
    }
    schedule();
}

void fileCallBack(void(*calldata), int result) {
    struct TCB* tcb = (TCB *) (calldata);
    tcb->result = result;
    tcb->tStatus = VM_THREAD_STATE_READY;
    pQ.at(tcb->tPriority).push(tcb);
    if(tcb->tPriority > curThread->tPriority) {
        schedule();
    }
}

void fileRepCallBack(void(*calldata), int result) {
    struct TCB* tcb = (TCB *) (calldata);
    tcb->result += result;
    pQ.at(tcb->tPriority).push(tcb);
    if(tcb->tPriority > curThread->tPriority) {
        schedule();
    }
}

TVMStatus VMFileOpenInt(const char *filename, int flags, int mode, int *filedescriptor) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    curThread->tStatus = VM_THREAD_STATE_WAITING;

    MachineFileOpen(filename, flags, mode, &fileCallBack, curThread);
    schedule();
    *filedescriptor = curThread->result;
    MachineResumeSignals(&signalState);
    return curThread->result < 0 ? VM_STATUS_FAILURE : VM_STATUS_SUCCESS;
}

TVMStatus VMFileReadInt(int filedescriptor, void *data, int *length) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    curThread->result = 0;
    curThread->tStatus = VM_THREAD_STATE_WAITING;

    if (data == NULL || length == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    int reps = *length/512 + (*length%512 == 0 ? 0 : 1);
    if(globalMem.used + reps >= 32) globalMem.used = 0;
    globalMem.used += reps;
    for(int i = 0; i < reps; i++) {
        MachineFileRead(filedescriptor, globalMem.frags.at(globalMem.used - reps+i),
                        ((i==reps-1 && *length%512!=0) ?*length%512 : 512), &fileRepCallBack, curThread);
    }
    schedule();
    for(int i = 0; i < reps; i++) {
        memcpy((char*)data+(512*i), globalMem.frags.at(globalMem.used - reps+i),
        ((i==reps-1 && *length%512!=0) ?*length%512 : 512));
    }
    globalMem.used++;

    *length = curThread->result;
    MachineResumeSignals(&signalState);
    return curThread->result < 0 ? VM_STATUS_FAILURE : VM_STATUS_SUCCESS;
}

TVMStatus VMFileWriteInt(int filedescriptor, void *data, int *length) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    curThread->result = 0;
    curThread->tStatus = VM_THREAD_STATE_WAITING;

    if (data == NULL || length == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    int reps = *length/512 + (*length%512 == 0 ? 0 : 1);
    if(globalMem.used + reps >= 32) globalMem.used = 0;
    for(int i = 0; i < reps; i++) {
        memcpy(globalMem.frags.at(globalMem.used), (char*)data+(512*i),((i==reps-1 && *length%512!=0) ?*length%512 : 512));
        globalMem.used++;
    }

    for(int i = 0; i < reps; i++) {
        MachineFileWrite(filedescriptor, globalMem.frags.at(globalMem.used - reps+i),
                         ((i==reps-1 && *length%512!=0) ?*length%512 : 512), &fileRepCallBack, curThread);
    }
    globalMem.used++;
    schedule();
    *length = curThread->result;
    MachineResumeSignals(&signalState);
    return curThread->result < 0 ? VM_STATUS_FAILURE : VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeekInt(int filedescriptor, int offset, int whence, int *newoffset) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    curThread->tStatus = VM_THREAD_STATE_WAITING;

    MachineFileSeek(filedescriptor, offset, whence, &fileCallBack, curThread);
    schedule();
    *newoffset = curThread->result;
    MachineResumeSignals(&signalState);
    return curThread->result < 0 ? VM_STATUS_FAILURE : VM_STATUS_SUCCESS;
}

int findFirstFree() {
    for(int i = 2; i < FAT.size(); i++) {
        if(FAT.at(i) == 0) return i;
    }
    return -1;
}

int firstFreeEntry() {
    int f = 0;
    for(auto & i : Root){
        if((int)i.DIR_Name[0] == 0 || (int)i.DIR_Name[0] == 0xE5) return f;
        f++;
    }
    return -1;
}

int nJumps(int n, int clusNo) {
    int pos = clusNo;
    for(int i = 0; i < n; i++) {
        if(FAT.at(pos) == 0xFFF8) break;
        pos = FAT.at(pos);
    }
    return pos;
}

TVMStatus ReadSector(int secNo, void* data) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    VMMutexAcquire(FATmtx, VM_TIMEOUT_INFINITE);
    int offset = 0;
    VMFileSeekInt(FATfd, secNo*512, SEEK_SET, &offset);
    //std::cout << offset << "\n";
    int len = 512;
    VMFileReadInt(FATfd, data, &len);
    VMMutexRelease(FATmtx);
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus ReadCluster(int clusterNo, void* data) {
    int clus = (clusterNo - 2)*bpb.BPB_SecPerClus + bpb.firstDataSector;
    for(int i = 0; i < bpb.BPB_SecPerClus; i++) {
        ReadSector(clus + i, (char*)data+(512*i));
    }
    return VM_STATUS_SUCCESS;
}

TVMStatus ReadFAT() {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    FAT.resize(bpb.BPB_FATSz16*256);
    for(int i = 0; i < bpb.BPB_FATSz16; i++) {
        ReadSector(bpb.BPB_RsvdSecCnt + i, FAT.data()+(i*256));
    }
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}


TVMStatus ReadRoot() {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    Root.resize(bpb.BPB_RootEntCnt);
    for(int i = 0; i < bpb.rootDirectorySectors; i++) {
        ReadSector(bpb.firstRootSector+i, Root.data()+(i*16));
    }
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}



TVMStatus Mount(const char* mount) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    VMFileOpenInt(mount, O_RDWR, 0600, &FATfd);
    int blen = 62;
    VMFileReadInt(FATfd, (void*)&bpb, &blen);
    //std::cout << blen << "\n";
    bpb.firstRootSector = bpb.BPB_RsvdSecCnt + bpb.BPB_NumFATs * bpb.BPB_FATSz16;
    bpb.rootDirectorySectors  = (bpb.BPB_RootEntCnt * 32) / 512;
    bpb.firstDataSector  = bpb.firstRootSector + bpb.rootDirectorySectors;
    bpb.clusterCount  = (bpb.BPB_TotSec32 - bpb.firstDataSector) / bpb.BPB_SecPerClus;
    //Read FAT
    ReadFAT();
    //Read Root
    ReadRoot();
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus WriteSector(int secNo, void* data) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    VMMutexAcquire(FATmtx, VM_TIMEOUT_INFINITE);
    int offset = 0;
    VMFileSeekInt(FATfd, secNo*512, SEEK_SET, &offset);
    int len = 512;
    VMFileWriteInt(FATfd, data, &len);
    VMMutexRelease(FATmtx);
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}
TVMStatus WriteCluster(Cluster* cluster) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    int clus = (cluster->clusNo - 2)*bpb.BPB_SecPerClus + bpb.firstDataSector;
    for(int i = 0; i < bpb.BPB_SecPerClus; i++) {
        WriteSector(clus + i, cluster->clusData.data()+(512*i));
    }
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus WriteFAT() {
    for(int i = 0; i < bpb.BPB_FATSz16; i++) {
        WriteSector(bpb.BPB_RsvdSecCnt + i, FAT.data()+(i*256));
    }
}

TVMStatus WriteRoot() {
    for(int i = 0; i < bpb.rootDirectorySectors; i++) {
        WriteSector(bpb.firstRootSector+i, Root.data()+(i*16));
    }
}

TVMStatus Unmount() {
    for(auto & File : Files) {
        for(auto & cluster : File.clusters) {
            if(cluster.dirty) {
                WriteCluster(&cluster);
            }
        }
    }
    WriteFAT();
    WriteRoot();
    return VM_STATUS_SUCCESS;
}


TVMStatus VMStart(int tickms, TVMMemorySize sharedsize, const char *mount, int argc, char *argv[]) {
    tickGl = tickms;
    path.push_back('/');
    allThreads.emplace_back(new TCB{0, NULL, NULL, 1, VM_THREAD_STATE_RUNNING, VM_THREAD_PRIORITY_NORMAL,0,0,NULL});
    curThread = allThreads.at(0);
    MachineContextSave(&allThreads.at(0)->context);



    pQ.at(0).emplace(new TCB{1, &idle, (void*)new uint8_t[0x1000000], 0x1000000, VM_THREAD_STATE_READY, 0,0,0,NULL});
    allThreads.push_back(pQ.at(0).front());
    MachineContextCreate(&allThreads.at(1)->context, &skelCreate,allThreads.at(1), allThreads.at(1)->stackBase,allThreads.at(1)->tMemorySize);

    TVMMainEntry entryPoint = VMLoadModule(argv[0]);
    if (entryPoint == NULL) return VM_STATUS_FAILURE;
    memLoc = MachineInitialize(sharedsize);
    MachineEnableSignals();

    for(int i = 0; (unsigned)i < sharedsize/512; i++) {
        globalMem.frags.push_back((char*)memLoc+(512*i));
    }

    VMMutexCreate(&FATmtx);
    Mount(mount);
    /*
    std::cout << "\n";
    for(auto i : bpb.BS_OEMName) {
        std::cout << (char)i;
    }
    std::cout << "\n";
    std::cout << (unsigned int)bpb.BPB_BytsPerSec<< "\n";
    std::cout << (unsigned int)bpb.BPB_SecPerClus<< "\n";
    std::cout << (unsigned int)bpb.BPB_RsvdSecCnt<< "\n";
    std::cout << (unsigned int)bpb.BPB_NumFATs<< "\n";
    std::cout << (unsigned int)bpb.BPB_RootEntCnt<< "\n";
    std::cout << (unsigned int)bpb.BPB_TotSec16<< "\n";
    std::cout << (unsigned int)bpb.BPB_Media<< "\n";
    std::cout << (unsigned int)bpb.BPB_FATSz16<< "\n";
    std::cout << (unsigned int)bpb.BPB_SecPerTrk<< "\n";
    std::cout << (unsigned int)bpb.BPB_NumHeads<< "\n";
    std::cout << (unsigned int)bpb.BPB_HiddSec<< "\n";
    std::cout << (unsigned int)bpb.BPB_TotSec32<< "\n";
    std::cout << (unsigned int)bpb.BS_DrvNum<< "\n";
    std::cout << (unsigned int)bpb.BS_Reserved1<< "\n";
    std::cout << (unsigned int)bpb.BS_BootSig<< "\n";
    std::cout << (unsigned int)bpb.BS_VolID<< "\n";
    for(auto i : bpb.BS_VolLab) {
        std::cout << (char)i;
    }
    std::cout << "\n";
    for(auto i : bpb.BS_FilSysType) {
        std::cout << (char)i;
    }
    std::cout << "\n";

    std::cout << bpb.rootDirectorySectors<< "\n";//  = (BPB_RootEntCnt * 32) / 512;
    std::cout << bpb.firstRootSector<< "\n";// = BPB_RsvdSecCnt + BPB_NumFATs * BPB_FATSz16;
    std::cout << bpb.firstDataSector<< "\n";//  = firstRootSector + rootDirectorySectors;
    std::cout << bpb.clusterCount<< "\n";//

    for(int i = 2; i < FAT.size(); i++){
        if ((unsigned int)FAT.at(i) == 0) break;
        std::cout << std::hex << FAT.at(i) << " ";
        if(i%8 == 0) std::cout << "\n";
    }
    std::cout << "\n";
    */
    for(auto & i : Root){
        if((int)i.DIR_Name[0] == 0) break;
        if(i.DIR_Attr&0x02) continue;
        for(int j = 0; j < 11; j++) {
            std::cout << (char)i.DIR_Name[j];
        }
        std::cout << "\n";
    }

    MachineRequestAlarm((useconds_t)tickGl*1000, &MRACallback, NULL);
    entryPoint(argc, argv);
    Unmount();
    MachineTerminate();
    VMUnloadModule();
    
    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickMS(int *tickmsref) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if (tickmsref == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *tickmsref = tickGl;
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if (tickref == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *tickref = tickCnt;
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus
VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if (entry == NULL || tid == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *tid = allThreads.size();
    allThreads.emplace_back(new TCB{*tid, entry,(void*)new uint8_t[memsize], memsize,
                                    VM_THREAD_STATE_DEAD, prio,0,0,param});
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadDelete(TVMThreadID thread) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    int i = -1;
    for(int i1 = 0; i1 < (signed)allThreads.size(); i1++) {
        if(allThreads.at(i1)->tID == thread) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    } else if (allThreads.at(i)->tStatus != VM_THREAD_STATE_DEAD){
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    delete allThreads.at(i)->stackBase;
    allThreads.erase(allThreads.begin() + i);
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadActivate(TVMThreadID thread) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    int i = -1;
    for(int i1 = 0; i1 < (signed)allThreads.size(); i1++) {
        if(allThreads.at(i1)->tID == thread) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    } else if (allThreads.at(i)->tStatus != VM_THREAD_STATE_DEAD){
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    allThreads.at(i)->tStatus = VM_THREAD_STATE_READY;
    pQ.at(allThreads.at(i)->tPriority).push(allThreads.at(i));
    MachineContextCreate(&allThreads.at(i)->context, &skelCreate, allThreads.at(i), allThreads.at(i)->stackBase,allThreads.at(i)->tMemorySize);
    if(allThreads.at(i)->tPriority > curThread->tPriority) {
        schedule();
    }
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID thread) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);

    int i = -1;
    for(int i1 = 0; i1 < (signed)allThreads.size(); i1++) {
        if(allThreads.at(i1)->tID == thread) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if (allThreads.at(i)->tStatus == VM_THREAD_STATE_DEAD){
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }

    allThreads.at(i)->tStatus = VM_THREAD_STATE_DEAD;
    schedule();
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadID(TVMThreadIDRef threadref) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if (threadref == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *threadref = curThread->tID;
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if (stateref == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    int i = -1;
    for(int i1 = 0; i1 < (signed)allThreads.size(); i1++) {
        if(allThreads.at(i1)->tID == thread) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    *stateref = allThreads.at(i)->tStatus;
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadSleep(TVMTick tick) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if (tick == VM_TIMEOUT_INFINITE) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    } else if (tick == VM_TIMEOUT_IMMEDIATE) {
        schedule();
    } else {
        curThread->sleepLen = tick;
        curThread->tStatus = VM_THREAD_STATE_WAITING;
        sleepers.push_back(curThread);
        schedule();
    }
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if(filename == nullptr || filedescriptor == nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }

    std::string fileIn(filename);
    transform(fileIn.begin(), fileIn.end(), fileIn.begin(), ::toupper);
    fileIn.erase(std::remove(fileIn.begin(), fileIn.end(), '/'), fileIn.end());

    //char out[VM_FILE_SYSTEM_MAX_PATH];
    //VMFileSystemGetAbsolutePath(&out, &cur_dir, dirname);
    //VMFileSystemFileFromFullPath(cleanFile, filename);
    *filedescriptor = fds.size() + 7;
    fds.push_back(*filedescriptor);
    Files.emplace_back();
    //memcpy(Files.at(*filedescriptor-7).fDir.DShortFileName,fileIn.c_str(), sizeof(*fileIn.c_str()));

    bool match = false;
    int i = 0;
    for(; i < Root.size(); i++) {
        if(Root.at(i).DIR_Name[0] == 0) continue;
        if(Root.at(i).DIR_Name[0] == 0xE5) break;
        if(Root.at(i).DIR_Attr&0x02) continue;
        std::string filnm;
        for(int j = 0; j < 8; j++) {
            if(Root.at(i).DIR_Name[j] != ' ') {
                filnm.push_back(Root.at(i).DIR_Name[j]);
            }
        }
        if(Root.at(i).DIR_Name[8] != ' ') {
            filnm.push_back('.');
            for(int j = 8; j < 11; j++) {
                if(Root.at(i).DIR_Name[j] != ' ') {
                    filnm.push_back(Root.at(i).DIR_Name[j]);
                }
            }
        }

        if(fileIn == filnm) {
            match = true;
            break;
        }
    }
    std::string longN = fileIn;
    if(match) {
        //CHECK READ ONLY
        Files.at(*filedescriptor-7).fileInRoot = &Root.at(i);
        Files.at(*filedescriptor-7).firstClus = Root.at(i).DIR_FstClusLo;
        memcpy(Files.at(*filedescriptor-7).fDir.DLongFileName, longN.c_str(), longN.size()*sizeof(char));
        
        //Files.at(*filedescriptor-7).fDir.DAttributes = Root.at(i).DIR_Attr;
        //Files.at(Files.size()-1).fDir.DAccess = Root.at(i).DIR_LstAccDate;
        //Files.at(Files.size()-1).fDir.DCreate
        
        
    } else {
        
        int period = fileIn.find('.');
        if (period != std::string::npos) {
            fileIn.resize(13);
            fileIn.at(period) = ' ';
            fileIn.at(8) = fileIn.at(period+1);
            fileIn.at(9) = fileIn.at(period+2);
            fileIn.at(10) = fileIn.at(period+3);
            for(int p = period; p < 8; p++) {
                fileIn.at(p) = ' ';
            }
            while(fileIn.size() > 11){
                fileIn.pop_back();
            }
        }
        
        
        Files.at(*filedescriptor-7).fileInRoot = &Root.at(firstFreeEntry());
        memcpy(Files.at(*filedescriptor-7).fDir.DShortFileName,fileIn.c_str(), fileIn.size()*sizeof(char));
        memcpy(Files.at(*filedescriptor-7).fileInRoot->DIR_Name,fileIn.c_str(), fileIn.size()*sizeof(char));
        
        Files.at(*filedescriptor-7).firstClus = findFirstFree();
        std::cout << findFirstFree() << std::endl;
        FAT.at(Files.at(*filedescriptor-7).firstClus) = 0xFFF8;
        int endClus = findFirstFree();
        FAT.at(endClus) = 0xFFF8;
        std::cout << findFirstFree() << std::endl;
        FAT.at(Files.at(*filedescriptor-7).firstClus) = endClus;
        SVMDateTime newCreate;
        VMDateTime(&newCreate);
        
        uint16_t yr = (newCreate.DYear)<<9;
        uint16_t mth = (newCreate.DMonth)<<5;
        uint16_t dy = newCreate.DDay;
        uint16_t hr = (newCreate.DHour)<<11;
        uint16_t mn = (newCreate.DMinute)<<5;
        uint16_t s = newCreate.DSecond;
        uint16_t hth = newCreate.DHundredth;


        Files.at(*filedescriptor-7).fileInRoot->DIR_Attr = flags;
        memcpy(Files.at(*filedescriptor-7).fDir.DLongFileName, longN.c_str(), longN.size()*sizeof(char));
        Files.at(*filedescriptor-7).fileInRoot->DIR_NTRes = 0;
        Files.at(*filedescriptor-7).fileInRoot->DIR_CrtTimeTenth = hth*10/2;
        Files.at(*filedescriptor-7).fileInRoot->DIR_CrtTime = (hr | mn | s);
        Files.at(*filedescriptor-7).fileInRoot->DIR_CrtDate = (yr | mth | dy);
        Files.at(*filedescriptor-7).fileInRoot->DIR_LstAccDate = Files.at(*filedescriptor-7).fileInRoot->DIR_CrtDate;
        Files.at(*filedescriptor-7).fileInRoot->DIR_FstClusHI = 0;
        Files.at(*filedescriptor-7).fileInRoot->DIR_WrtTime = Files.at(*filedescriptor-7).fileInRoot->DIR_CrtTime;
        Files.at(*filedescriptor-7).fileInRoot->DIR_WrtDate = Files.at(*filedescriptor-7).fileInRoot->DIR_CrtDate;
        Files.at(*filedescriptor-7).fileInRoot->DIR_FileSize = 0;

    }

    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileClose(int filedescriptor) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    fds.at(filedescriptor-7) = -1;

    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    
    if(filedescriptor < 3) {
        VMFileReadInt(filedescriptor,data,length);
        MachineResumeSignals(&signalState);
        return VM_STATUS_SUCCESS;
    }
    if (data == NULL || length == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!(std::count(fds.begin(), fds.end(), filedescriptor))) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    
    SVMDateTime newAccess;
    VMDateTime(&newAccess);
    Files.at(filedescriptor-7).fDir.DAccess = newAccess;

    int clusterNo = nJumps(Files.at(filedescriptor-7).filePos/(512*bpb.BPB_SecPerClus), Files.at(filedescriptor-7).firstClus);
    int clusOffset = Files.at(filedescriptor-7).filePos%(512*bpb.BPB_SecPerClus);
    bool inCache = false;
    int clusPos = 0;
    if(!Files.at(filedescriptor-7).clusters.empty()) {
        for (auto &cluster : Files.at(filedescriptor - 7).clusters) {
            if (cluster.clusNo == clusterNo) {
                inCache = true;
                break;
            }
            clusPos++;
        }
    }
    if(inCache) {
        if(*length+clusOffset > 512){}
    } else {
        Files.at(filedescriptor-7).clusters.emplace_back(clusterNo);
        clusPos = Files.at(filedescriptor-7) .clusters.size()-1;
        ReadCluster(clusterNo,Files.at(filedescriptor-7).clusters.at(clusPos).clusData.data());
    }
    
    if(Files.at(filedescriptor-7).filePos + *length > Files.at(filedescriptor-7).fDir.DSize) {*length = Files.at(filedescriptor-7).fDir.DSize-Files.at(filedescriptor-7).filePos;}
    memcpy(data, Files.at(filedescriptor-7).clusters.at(clusPos).clusData.data()+clusOffset, *length);
    

    uint16_t yr = ((newAccess.DYear)<<9) - 1980;
    uint16_t mth = (newAccess.DMonth)<<5;
    uint16_t dy = newAccess.DDay;

    Files.at(filedescriptor-7).fileInRoot->DIR_LstAccDate = ((yr | mth) | dy);
    
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    
    if(filedescriptor < 3) {
        VMFileWriteInt(filedescriptor,data,length);
        MachineResumeSignals(&signalState);
        return VM_STATUS_SUCCESS;
    }
    if (data == NULL || length == NULL) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!(std::count(fds.begin(), fds.end(), filedescriptor))) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    
    SVMDateTime newAccess;
    VMDateTime(&newAccess);
    Files.at(filedescriptor-7).fDir.DAccess = newAccess;

    int clusterNo = nJumps(Files.at(filedescriptor-7).filePos/(512*bpb.BPB_SecPerClus), Files.at(filedescriptor-7).firstClus);
    int clusOffset = Files.at(filedescriptor-7).filePos%(512*bpb.BPB_SecPerClus);
    bool inCache = false;
    int clusPos = 0;
    if(!Files.at(filedescriptor-7).clusters.empty()) {
        for (auto &cluster : Files.at(filedescriptor - 7).clusters) {
            if (cluster.clusNo == clusterNo) {
                inCache = true;
                break;
            }
            clusPos++;
        }
    }
    if(inCache) {
        if(*length+clusOffset > 512){}

    } else {
        Files.at(filedescriptor-7).clusters.emplace_back(clusterNo);
        clusPos = Files.at(filedescriptor-7).clusters.size()-1;
        ReadCluster(clusterNo,Files.at(filedescriptor-7).clusters.at(clusPos).clusData.data());
    }
    memcpy(Files.at(filedescriptor-7).clusters.at(clusPos).clusData.data()+clusOffset, data, *length);
    Files.at(filedescriptor-7).clusters.at(clusPos).dirty = true;
    
    if(Files.at(filedescriptor-7).filePos + 
    *length > Files.at(filedescriptor-7).fDir.DSize) Files.at(filedescriptor-7).fDir.DSize += *length;

    uint16_t yr = ((newAccess.DYear)<<9) - 1980;
    uint16_t mth = (newAccess.DMonth)<<5;
    uint16_t dy = newAccess.DDay;
    uint16_t hr = (newAccess.DHour)<<11;
    uint16_t mn = (newAccess.DMinute)<<5;
    uint16_t s = newAccess.DSecond;
    uint16_t hth = newAccess.DHundredth;

    Files.at(filedescriptor-7).fileInRoot->DIR_LstAccDate = ((yr | mth) | dy);
    Files.at(filedescriptor-7).fileInRoot->DIR_WrtTime = ((hr | mn )| s);
    Files.at(filedescriptor-7).fileInRoot->DIR_WrtDate = ((yr | mth )| dy);

    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    //curThread->tStatus = VM_THREAD_STATE_WAITING;

    if (!(std::count(fds.begin(), fds.end(), filedescriptor))) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }

    SVMDateTime newAccess;
    VMDateTime(&newAccess);
    Files.at(filedescriptor-7).fDir.DAccess = newAccess;

    if(whence == 0) {
        Files.at(filedescriptor-7).filePos = offset;
        *newoffset = offset;
    } else if(whence == 1) {
        Files.at(filedescriptor-7).filePos += offset;
        *newoffset = Files.at(filedescriptor-7).filePos;
    } else if(whence == 2) {
        Files.at(filedescriptor-7).filePos = Files.at(filedescriptor-7).fDir.DSize + offset;
        *newoffset = Files.at(filedescriptor-7).filePos;
    }

    //schedule();
    //*newoffset = curThread->result;
    MachineResumeSignals(&signalState);
    return *newoffset < 0 ? VM_STATUS_FAILURE : VM_STATUS_SUCCESS;
}

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if(mutexref == nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *mutexref = mtx.size();
    mtx.emplace_back(new Mutex{nullptr, *mutexref});
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutex) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    int i = -1;
    for(int i1 = 0; i1 < (signed)mtx.size(); i1++) {
        if(mtx.at(i1)->mID == mutex) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if(mtx.at(i)->owner != nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    //DELETE
    mtx.erase(mtx.begin() + i);
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if(ownerref == nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    int i = -1;
    for(int i1 = 0; i1 < (signed)mtx.size(); i1++) {
        if(mtx.at(i1)->mID == mutex) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if(mtx.at(i)->owner == nullptr) {
        *ownerref = VM_THREAD_ID_INVALID;
    } else {
        *ownerref = mtx.at(1)->owner->tID;
    }
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    int i = -1;
    for(int i1 = 0; i1 < (signed)mtx.size(); i1++) {
        if(mtx.at(i1)->mID == mutex) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    //Aquire
    if(mtx.at(i)->owner == nullptr) {
        mtx.at(i)->owner = curThread;
    } else {
        if(timeout == VM_TIMEOUT_IMMEDIATE) {
            MachineResumeSignals(&signalState);
            return VM_STATUS_FAILURE;
        } else if(timeout == VM_TIMEOUT_INFINITE) {
            mtx.at(i)->wQ.at(curThread->tPriority).push(curThread);
            curThread->tStatus = VM_THREAD_STATE_WAITING;
            schedule();
            /*while(mtx.at(i)->owner != curThread) {
                VMThreadSleep(1);
            }*/
        } else {
            mtx.at(i)->wQ.at(curThread->tPriority).push(curThread);
            VMThreadSleep(timeout);
            if(mtx.at(i)->owner != curThread) {
                //Manual popout
                std::queue<struct TCB*> temp;
                while(true) {
                    temp.push(mtx.at(i)->wQ.at(curThread->tPriority).front());
                    mtx.at(i)->wQ.at(curThread->tPriority).pop();
                    if(temp.front() == curThread) break;
                }
                temp.pop();
                for(int j = temp.size(); j >= 0 ; j--) {
                    mtx.at(i)->wQ.at(curThread->tPriority).push(temp.front());
                    temp.pop();
                }
                MachineResumeSignals(&signalState);
                return VM_STATUS_FAILURE;
            }
        }
    }

    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexRelease(TVMMutexID mutex) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    int i = -1;
    for(int i1 = 0; i1 < (signed)mtx.size(); i1++) {
        if(mtx.at(i1)->mID == mutex) {
            i = i1;
            break;
        }
    }
    if(i == -1) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if(mtx.at(i)->owner != curThread) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }

    struct TCB* next = nullptr;

    if(!mtx.at(i)->wQ.at(3).empty()) {
        next = mtx.at(i)->wQ.at(3).front();
        mtx.at(i)->wQ.at(3).pop();
    } else if(!mtx.at(i)->wQ.at(2).empty()) {
        next = mtx.at(i)->wQ.at(2).front();
        mtx.at(i)->wQ.at(2).pop();
    } else if(!mtx.at(i)->wQ.at(1).empty()) {
        next = mtx.at(i)->wQ.at(1).front();
        mtx.at(i)->wQ.at(1).pop();
    }

    mtx.at(i)->owner = next;
    if(mtx.at(i)->owner) {
        int j = -1;
        for(int j1 = 0; j1 < (signed)sleepers.size(); j1++) {
            if(sleepers.at(j1) == mtx.at(i)->owner) {
                j = j1;
                break;
            }
        }
        if(j >= 0) {
            sleepers.at(j)->sleepLen = 0;
            //sleepers.at(j)->tStatus = VM_THREAD_STATE_READY;
            //pQ.at(sleepers.at(j)->tPriority).push(sleepers.at(j));
            sleepers.erase(sleepers.begin() + j);
        }
        mtx.at(i)->owner->tStatus = VM_THREAD_STATE_READY;
        pQ.at(mtx.at(i)->owner->tPriority).push(mtx.at(i)->owner);
        if(mtx.at(i)->owner->tPriority > curThread->tPriority) {
            schedule();
        }
    }

    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}



TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if(dirname == nullptr || dirdescriptor == nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }


    //char out[VM_FILE_SYSTEM_MAX_PATH];
    //VMFileSystemGetAbsolutePath(&out, &cur_dir, dirname);
    //VMFileSystemFileFromFullPath(cleanFile, filename);

    *dirdescriptor = fds.size() + 7;
    dds.push_back(*dirdescriptor);
    Dirs.emplace_back();
    
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMDirectoryClose(int dirdescriptor) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    dds.at(dirdescriptor-7) = -1;

    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}
/*
TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if(dirent == nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    int next = 0;
    for(auto & i : Root){
        if((int)i.DIR_Name[0] == 0) break;
        if(i.DIR_Attr&0x02) continue;
        if(next !=  Dirs.at(dirdescriptor-7).filePos) {
            next++;
            continue;
        } else {break;}
        
    }
        memcpy(dirent->DShortFileName, Root.at(next).DIR_Name, 13);
        dirent->DSize =  Root.at(next).DIR_FileSize;
        dirent->DAttributes = Root.at(next).DIR_Attr;
        dirent->DModify.DDay = (Root.at(next).DIR_WrtDate<<11)>>11;
        dirent->DModify.DMonth = ((Root.at(next).DIR_WrtDate)<<7)>>12;
        dirent->DModify.DYear = ((Root.at(next).DIR_WrtDate)>>9) + 1980;
        dirent->DModify.DHour = (Root.at(next).DIR_WrtTime)>>11;
        dirent->DModify.DMinute = ((Root.at(next).DIR_WrtTime)<<10)>>10;
    Dirs.at(dirdescriptor-7).filePos++;
    MachineResumeSignals(&signalState);
    return VM_STATUS_SUCCESS;
}*/
TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent) {
    TMachineSignalState signalState;
    MachineSuspendSignals(&signalState);
    if(dirent == nullptr) {
        MachineResumeSignals(&signalState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    
    int next = Dirs.at(dirdescriptor-7).filePos;
    if(Root.at(next).DIR_Attr&0x02) next++;
    
    memcpy(dirent->DShortFileName, Root.at(next).DIR_Name, 13);
    dirent->DSize =  Root.at(next).DIR_FileSize;
    dirent->DAttributes = Root.at(next).DIR_Attr;
    dirent->DModify.DDay = ((unsigned)(Root.at(next).DIR_WrtDate)<<10)>>10;
    dirent->DModify.DMonth = ((unsigned)(Root.at(next).DIR_WrtDate)<<7)>>12;
    dirent->DModify.DYear = ((unsigned)(Root.at(next).DIR_WrtDate)>>9) + 1980;
    dirent->DModify.DHour = ((unsigned)Root.at(next).DIR_WrtTime)>>11;
    dirent->DModify.DMinute = ((unsigned)(Root.at(next).DIR_WrtTime)<<10)>>10;
    Dirs.at(dirdescriptor-7).filePos++;
    MachineResumeSignals(&signalState);
    return Root.at(Dirs.at(dirdescriptor-7).filePos).DIR_Name[0] == 0 ? VM_STATUS_FAILURE : VM_STATUS_SUCCESS;
}

TVMStatus VMDirectoryRewind(int dirdescriptor) {
    return 0;
}

TVMStatus VMDirectoryCurrent(char *abspath) {
    //memcpy(abspath,path.c_str(),path.size()*sizeof(char));
    abspath[0] = '/';
    abspath[1] = '\0';
    return VM_STATUS_SUCCESS;
}

TVMStatus VMDirectoryChange(const char *path) {
    return 0;
}

}