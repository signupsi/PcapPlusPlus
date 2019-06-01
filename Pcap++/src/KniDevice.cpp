#if defined(USE_DPDK) && defined(LINUX)

#define LOG_MODULE PcapLogModuleKniDevice

#include "KniDevice.h"
#include "Logger.h"
#include "SystemUtils.h"

#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>

#include <rte_version.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_kni.h>
#include <rte_memory.h>
#include <rte_branch_prediction.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>

#define KNI_MEMPOOL_NAME_PREFIX "kniMempool"
#define MEMPOOL_CACHE_SIZE 256
#define MAX_BURST_SIZE 64

#define CPP_VLA(TYPE, SIZE) (TYPE*)__builtin_alloca(sizeof(TYPE) * SIZE)

namespace pcpp
{

/**
 * ==========================
 * Class KniDevice::KniThread
 * ==========================
 */

struct KniDevice::KniThread
{
	enum KniThreadCleanupState
	{
		JOINABLE,
		DETACHED,
		INVALID
	};
	typedef void*(*threadMain)(void*);
	KniThread(KniThreadCleanupState s, threadMain tm, void* data);
	~KniThread();

	bool cancel();

	pthread_t m_Descriptor;
	KniThreadCleanupState m_State;
};

KniDevice::KniThread::KniThread(KniThreadCleanupState s, threadMain tm, void* data) :
	m_State(s)
{
	int err = pthread_create(&m_Descriptor, NULL, tm, data);
	if (err != 0)
	{
		const char* errs = std::strerror(err);
		LOG_ERROR("KNI can't start pthread. pthread_create returned an error: %s", errs);
		m_State = INVALID;
		return;
	}
	if (m_State == DETACHED)
	{
		err = pthread_detach(m_Descriptor);
		if (err != 0)
		{
			const char* errs = std::strerror(err);
			LOG_ERROR("KNI can't detach pthread. pthread_detach returned an error: %s", errs);
			m_State = INVALID;
			return;
		}
	}
}

KniDevice::KniThread::~KniThread()
{
	if (m_State == JOINABLE)
	{
		int err = pthread_join(m_Descriptor, NULL);
		if (err != 0)
		{
			const char* errs = std::strerror(err);
			LOG_DEBUG("KNI failed to join pthread. pthread_join returned an error: %s", errs);
		}
	}
}

bool KniDevice::KniThread::cancel()
{
	return pthread_cancel(m_Descriptor);
}

/**
 * ===============
 * Class KniDevice
 * ===============
 */

namespace
{

inline bool destroyKniDevice(struct rte_kni* kni, const char* deviceName)
{
	if (rte_kni_release(kni) < 0)
	{
		LOG_ERROR("Failed to destroy DPDK KNI device %s", deviceName);
		return true;
	}
	return false;
}

inline KniDevice::KniLinkState setKniDeviceLinkState(
	struct rte_kni* kni,
	const char* deviceName,
	KniDevice::KniLinkState state = KniDevice::LINK_UP
)
{
	KniDevice::KniLinkState oldState = KniDevice::LINK_NOT_SUPPORTED;
	if (kni == NULL || !(state == KniDevice::LINK_UP || state == KniDevice::LINK_DOWN))
	{
		return oldState = KniDevice::LINK_ERROR;
	}
	#if RTE_VERSION >= RTE_VERSION_NUM(18, 11, 0, 0)
		oldState = (KniDevice::KniLinkState)rte_kni_update_link(kni, state);
		if (oldState == KniDevice::LINK_ERROR)
		{	//? NOTE(echo-Mike): Not LOG_ERROR because will generate a lot of junk messages on some DPDK versions
			LOG_DEBUG("DPDK KNI Failed to update links state for device \"%s\"", deviceName);
		}
	#else
		// To avoid compiler warnings
		(void) kni;
		(void) deviceName;
	#endif
	return oldState;
}

inline struct rte_mempool* createMempool(size_t mempoolSize, int unique, const char* deviceName)
{
	struct rte_mempool* result = NULL;
	char mempoolName[64];
	snprintf(mempoolName, sizeof(mempoolName),
		KNI_MEMPOOL_NAME_PREFIX "%d",
		unique
	);
	result = rte_pktmbuf_pool_create(
		mempoolName,
		mempoolSize,
		MEMPOOL_CACHE_SIZE,
		0,
		RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id()
	);
	if (result == NULL)
	{
		LOG_ERROR("Failed to create packets memory pool for KNI device %s, pool name: %s", deviceName, mempoolName);
	}
	else
	{
		LOG_DEBUG("Successfully initialized packets pool of size [%lu] for KNI device [%s]", (unsigned long)mempoolSize, deviceName);
	}
	return result;
}

} // namespace

KniDevice::KniDevice(const KniDeviceConfiguration& conf, size_t mempoolSize, int unique) :
	m_Device(NULL), m_MBufMempool(NULL)
{
	struct rte_kni_ops kniOps;
	struct rte_kni_conf kniConf;
	m_DeviceInfo.init(conf);
	m_Requests.thread = NULL;
	std::memset(&m_Capturing, 0, sizeof(m_Capturing));
	std::memset(&m_Requests, 0, sizeof(m_Requests));

	if ((m_MBufMempool = createMempool(mempoolSize, unique, conf.name)) == NULL)
		return;

	std::memset(&kniOps, 0, sizeof(kniOps));
	std::memset(&kniConf, 0, sizeof(kniConf));
	snprintf(kniConf.name, RTE_KNI_NAMESIZE, "%s", conf.name);
	kniConf.core_id = conf.kthreadCoreId;
	kniConf.mbuf_size = MBufRawPacket::MBUF_DATA_SIZE;
	kniConf.force_bind = conf.bindKthread ? 1 : 0;
#if RTE_VERSION >= RTE_VERSION_NUM(18, 2, 0, 0)
	if (conf.mac != NULL)
		conf.mac->copyTo((uint8_t*)kniConf.mac_addr);
	kniConf.mtu = conf.mtu;
#endif

	kniOps.port_id = conf.portId;
#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 0)
	if (conf.callbacks != NULL)
	{
		kniOps.change_mtu = conf.callbacks->change_mtu;
		kniOps.config_network_if = conf.callbacks->config_network_if;
	#if RTE_VERSION >= RTE_VERSION_NUM(18, 2, 0, 0)
		kniOps.config_mac_address = conf.callbacks->config_mac_address;
		kniOps.config_promiscusity = conf.callbacks->config_promiscusity;
	#endif
	}
#else
	if (conf.oldCallbacks != NULL)
	{
		kniOps.change_mtu = conf.oldCallbacks->change_mtu;
		kniOps.config_network_if = conf.oldCallbacks->config_network_if;
	}
#endif

	m_Device = rte_kni_alloc(m_MBufMempool, &kniConf, &kniOps);
	if (m_Device == NULL)
	{
		LOG_ERROR("DPDK have failed to create KNI device %s", conf.name);
	}
}

KniDevice::~KniDevice()
{
	m_Requests.cleanup();
	m_Capturing.cleanup();
	setKniDeviceLinkState(m_Device, m_DeviceInfo.name, KniDevice::LINK_DOWN);
	destroyKniDevice(m_Device, m_DeviceInfo.name);
}

void KniDevice::KniDeviceInfo::init(const KniDeviceConfiguration& conf)
{
	link = KniDevice::LINK_NOT_SUPPORTED;
	promisc = KniDevice::PROMISC_DISABLE;
	portId = conf.portId;
	mtu = conf.mtu;
	snprintf(name, sizeof(name), "%s", conf.name);
	mac = conf.mac != NULL ? *conf.mac : MacAddress::Zero;
}

KniDevice::KniLinkState KniDevice::getLinkState(KniInfoState state)
{
	if (state == KniDevice::INFO_CACHED)
		return m_DeviceInfo.link;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCGIFFLAGS, &req))
	{
		LOG_ERROR("DPDK KNI failed to obtain interface link state from Linux");
		LOG_DEBUG("Last known link state for device \"%s\" is returned", m_DeviceInfo.name);
		return m_DeviceInfo.link;
	}
	return m_DeviceInfo.link = KniLinkState(req.ifr_flags & IFF_UP);
}

MacAddress KniDevice::getMacAddress(KniInfoState state)
{
	if (state == KniDevice::INFO_CACHED)
		return m_DeviceInfo.mac;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	req.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCGIFHWADDR, &req))
	{
		LOG_ERROR("DPDK KNI failed to obtain MAC address from Linux");
		LOG_DEBUG("Last known MAC address for device \"%s\" is returned", m_DeviceInfo.name);
		return m_DeviceInfo.mac;
	}
	return m_DeviceInfo.mac = MacAddress((uint8_t*)req.ifr_hwaddr.sa_data);
}

uint16_t KniDevice::getMtu(KniInfoState state)
{
	if (state == KniDevice::INFO_CACHED)
		return m_DeviceInfo.mtu;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCGIFMTU, &req))
	{
		LOG_ERROR("DPDK KNI failed to obtain interface MTU from Linux");
		LOG_DEBUG("Last known MTU for device \"%s\" is returned", m_DeviceInfo.name);
		return m_DeviceInfo.mtu;
	}
	return m_DeviceInfo.mtu = req.ifr_mtu;
}

KniDevice::KniPromiscuousMode KniDevice::getPromiscuous(KniInfoState state)
{
	if (state == KniDevice::INFO_CACHED)
		return m_DeviceInfo.promisc;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCGIFFLAGS, &req))
	{
		LOG_ERROR("DPDK KNI failed to obtain interface Promiscuous mode from Linux");
		LOG_DEBUG("Last known Promiscuous mode for device \"%s\" is returned", m_DeviceInfo.name);
		return m_DeviceInfo.promisc;
	}
	return m_DeviceInfo.promisc = (req.ifr_flags & IFF_PROMISC) ? KniDevice::PROMISC_ENABLE : KniDevice::PROMISC_DISABLE;
}

bool KniDevice::setLinkState(KniLinkState state)
{
	if (!(state == KniDevice::LINK_DOWN || state == KniDevice::LINK_UP))
		return false;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCGIFFLAGS, &req))
	{
		LOG_ERROR("DPDK KNI failed to obtain interface flags from Linux");
		return false;
	}
	if ((state == KniDevice::LINK_DOWN && req.ifr_flags & IFF_UP) ||
		(state == KniDevice::LINK_UP && !(req.ifr_flags & IFF_UP)))
	{
		req.ifr_flags ^= IFF_UP;
		if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCSIFFLAGS, &req))
		{
			LOG_ERROR("DPDK KNI failed to set \"%s\" link mode", m_DeviceInfo.name);
			return false;
		}
	}
	m_DeviceInfo.link = state;
	return true;
}

bool KniDevice::setMacAddress(MacAddress mac)
{
	if (!mac.isValid())
		return false;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	req.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	mac.copyTo((uint8_t*)req.ifr_hwaddr.sa_data);
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCSIFHWADDR, &req))
	{
		LOG_ERROR("DPDK KNI failed to set MAC address");
		return false;
	}
	m_DeviceInfo.mac = mac;
	return true;
}

bool KniDevice::setMtu(uint16_t mtu)
{
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	req.ifr_mtu = mtu;
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCSIFMTU, &req))
	{
		LOG_ERROR("DPDK KNI failed to set interface MTU");
		return false;
	}
	m_DeviceInfo.mtu = mtu;
	return true;
}

bool KniDevice::setPromiscuous(KniPromiscuousMode mode)
{
	if (!(mode == KniDevice::PROMISC_DISABLE || mode == KniDevice::PROMISC_ENABLE))
		return false;
	struct ifreq req;
	std::memset(&req, 0, sizeof(req));
	if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCGIFFLAGS, &req))
	{
		LOG_ERROR("DPDK KNI failed to obtain interface flags from Linux");
		return false;
	}
	if ((mode == KniDevice::PROMISC_DISABLE && req.ifr_flags & IFF_PROMISC) ||
		(mode == KniDevice::PROMISC_ENABLE && !(req.ifr_flags & IFF_PROMISC)))
	{
		req.ifr_flags ^= IFF_PROMISC;
		if (!m_DeviceInfo.soc.makeRequest(m_DeviceInfo.name, SIOCSIFFLAGS, &req))
		{
			LOG_ERROR("DPDK KNI failed to set \"%s\" link mode", m_DeviceInfo.name);
			return false;
		}
	}
	m_DeviceInfo.promisc = mode;
	return true;
}

KniDevice::KniLinkState KniDevice::updateLinkState(KniLinkState state)
{
	KniLinkState oldState = setKniDeviceLinkState(m_Device, m_DeviceInfo.name, state);
	if (oldState != KniDevice::LINK_NOT_SUPPORTED && oldState != KniDevice::LINK_ERROR)
		m_DeviceInfo.link = state;
	return oldState;
}

bool KniDevice::handleRequests()
{
	return rte_kni_handle_request(m_Device) == 0;
}

void KniDevice::KniRequests::cleanup()
{
	if (thread)
		thread->cancel();
	delete thread;
	thread = NULL;
	sleepS = sleepNs = 0;
}

void* KniDevice::KniRequests::runRequests(void* devicePointer)
{
	KniDevice* device = (KniDevice*)devicePointer;
	struct timespec sleepTime;
	sleepTime.tv_sec = device->m_Requests.sleepS;
	sleepTime.tv_nsec = device->m_Requests.sleepNs;
	struct rte_kni* kni_dev = device->m_Device;
	for(;;)
	{
		nanosleep(&sleepTime, NULL);
		rte_kni_handle_request(kni_dev);
	}
	return NULL;
}

bool KniDevice::startRequestHandlerThread(long sleepSeconds, long sleepNanoSeconds)
{
	if (m_Requests.thread != NULL)
	{
		LOG_ERROR("KNI request thread is already started for device \"%s\"", m_DeviceInfo.name);
		return false;
	}
	m_Requests.sleepS = sleepSeconds;
	m_Requests.sleepNs = sleepNanoSeconds;
	m_Requests.thread = new KniThread(KniThread::DETACHED, KniRequests::runRequests, (void*)this);
	if (m_Requests.thread->m_State == KniThread::INVALID)
	{
		m_Requests.cleanup();
		return false;
	}
	return true;
}

void KniDevice::stopRequestHandlerThread()
{
	if (m_Requests.thread == NULL)
	{
		LOG_DEBUG("Attempt to stop not running KNI request thread for device \"%s\"", m_DeviceInfo.name);
		return;
	}
	m_Requests.cleanup();
}

uint16_t KniDevice::receivePackets(MBufRawPacketVector& rawPacketsArr)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}
	if (unlikely(m_Capturing.isRunning()))
	{
		LOG_ERROR(
			"KNI device \"%s\" capture mode is currently running. "
			"Cannot receive packets in parallel",
			m_DeviceInfo.name
		);
		return 0;
	}

	struct rte_mbuf* mBufArray[MAX_BURST_SIZE];
	uint32_t numOfPktsReceived = rte_kni_rx_burst(m_Device, mBufArray, MAX_BURST_SIZE);

	//the following line trashes the log with many messages. Uncomment only if necessary
	//LOG_DEBUG("KNI Captured %d packets", numOfPktsReceived);

	if (unlikely(numOfPktsReceived <= 0))
	{
		return 0;
	}

	timeval time;
	gettimeofday(&time, NULL);

	for (uint32_t index = 0; index < numOfPktsReceived; ++index)
	{
		struct rte_mbuf* mBuf = mBufArray[index];
		MBufRawPacket* newRawPacket = new MBufRawPacket();
		newRawPacket->setMBuf(mBuf, time);
		rawPacketsArr.pushBack(newRawPacket);
	}

	return numOfPktsReceived;
}

uint16_t KniDevice::receivePackets(MBufRawPacket** rawPacketsArr, uint16_t rawPacketArrLength)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}
	if (unlikely(m_Capturing.isRunning()))
	{
		LOG_ERROR(
			"KNI device \"%s\" capture mode is currently running. "
			"Cannot recieve packets in parallel",
			m_DeviceInfo.name
		);
		return 0;
	}
	if (unlikely(rawPacketsArr == NULL))
	{
		LOG_ERROR("KNI Provided address of array to store packets is NULL");
		return 0;
	}

	struct rte_mbuf** mBufArray = CPP_VLA(struct rte_mbuf*, rawPacketArrLength);
	uint16_t packetsReceived = rte_kni_rx_burst(m_Device, mBufArray, MAX_BURST_SIZE);

	//LOG_DEBUG("KNI Captured %d packets", rawPacketArrLength);

	if (unlikely(packetsReceived <= 0))
	{
		return 0;
	}

	timeval time;
	gettimeofday(&time, NULL);

	for (size_t index = 0; index < packetsReceived; ++index)
	{
		struct rte_mbuf* mBuf = mBufArray[index];
		if (rawPacketsArr[index] == NULL)
			rawPacketsArr[index] = new MBufRawPacket();

		((MBufRawPacket*)rawPacketsArr[index])->setMBuf(mBuf, time);
	}

	return packetsReceived;
}

uint16_t KniDevice::receivePackets(Packet** packetsArr, uint16_t packetsArrLength)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}
	if (unlikely(m_Capturing.isRunning()))
	{
		LOG_ERROR(
			"KNI device \"%s\" capture mode is currently running. "
			"Cannot recieve packets in parallel",
			m_DeviceInfo.name
		);
		return 0;
	}


	struct rte_mbuf** mBufArray = CPP_VLA(struct rte_mbuf*, packetsArrLength);
	uint16_t packetsReceived = rte_kni_rx_burst(m_Device, mBufArray, MAX_BURST_SIZE);

	//LOG_DEBUG("KNI Captured %d packets", packetsArrLength);

	if (unlikely(packetsReceived <= 0))
	{
		return 0;
	}

	timeval time;
	gettimeofday(&time, NULL);

	for (size_t index = 0; index < packetsReceived; ++index)
	{
		struct rte_mbuf* mBuf = mBufArray[index];
		MBufRawPacket* newRawPacket = new MBufRawPacket();
		newRawPacket->setMBuf(mBuf, time);
		if (packetsArr[index] == NULL)
			packetsArr[index] = new Packet();

		packetsArr[index]->setRawPacket(newRawPacket, true);
	}

	return packetsReceived;
}

uint16_t KniDevice::sendPackets(MBufRawPacket** rawPacketsArr, uint16_t arrLength)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}

	struct rte_mbuf** mBufArray = CPP_VLA(struct rte_mbuf*, arrLength);
	for (uint16_t i = 0; i < arrLength; ++i)
	{
		mBufArray[i] = rawPacketsArr[i]->getMBuf();
	}

	uint16_t packetsSent = rte_kni_tx_burst(m_Device, mBufArray, arrLength);
	for (uint16_t i = 0; i < arrLength; ++i)
	{
		rawPacketsArr[i]->setFreeMbuf(i >= packetsSent);
	}

	return packetsSent;
}

uint16_t KniDevice::sendPackets(Packet** packetsArr, uint16_t arrLength)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}

	struct rte_mbuf** mBufArray = CPP_VLA(struct rte_mbuf*, arrLength);
	MBufRawPacket** mBufRawPacketArr = CPP_VLA(MBufRawPacket*, arrLength);
	MBufRawPacket** allocated = CPP_VLA(MBufRawPacket*, arrLength);
	uint16_t allocated_count = 0, packetsSent = 0;
	MBufRawPacket* rawPacket;
	RawPacket* raw_pkt;

	for (uint16_t i = 0; i < arrLength; ++i)
	{
		raw_pkt = packetsArr[i]->getRawPacketReadOnly();
		uint8_t raw_type = raw_pkt->getObjectType();
		if (raw_type != MBUFRAWPACKET_OBJECT_TYPE)
		{
			MBufRawPacket* pkt = new MBufRawPacket();
			if (unlikely(!pkt->initFromRawPacket(raw_pkt, this)))
			{
				delete pkt;
				goto error_out;
			}
			rawPacket = allocated[allocated_count++] = pkt;
		}
		else
		{
			rawPacket = (MBufRawPacket*)raw_pkt;
		}
		mBufRawPacketArr[i] = rawPacket;
		mBufArray[i] = rawPacket->getMBuf();
	}

	packetsSent = rte_kni_tx_burst(m_Device, mBufArray, arrLength);
	for (uint16_t i = 0; i < arrLength; ++i)
	{
		mBufRawPacketArr[i]->setFreeMbuf(i >= packetsSent);
	}

error_out:
	for (uint16_t i = 0; i < allocated_count; ++i)
		delete allocated[i];
	return packetsSent;
}

uint16_t KniDevice::sendPackets(MBufRawPacketVector& rawPacketsVec)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}

	size_t arrLength = rawPacketsVec.size();
	struct rte_mbuf** mBufArray = CPP_VLA(struct rte_mbuf*, arrLength);
	uint16_t pos = 0;
	for (MBufRawPacketVector::VectorIterator iter = rawPacketsVec.begin(); iter != rawPacketsVec.end(); ++iter)
	{
		mBufArray[pos] = (*iter)->getMBuf();
		++pos;
	}

	uint16_t packetsSent = rte_kni_tx_burst(m_Device, mBufArray, arrLength);
	pos = 0;
	for (MBufRawPacketVector::VectorIterator iter = rawPacketsVec.begin(); iter != rawPacketsVec.end(); ++iter)
	{
		(*iter)->setFreeMbuf(pos >= packetsSent);
		++pos;
	}

	return packetsSent;
}

uint16_t KniDevice::sendPackets(RawPacketVector& rawPacketsVec)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}

	size_t arrLength = rawPacketsVec.size();
	struct rte_mbuf** mBufArray = CPP_VLA(struct rte_mbuf*, arrLength);
	MBufRawPacket** mBufRawPacketArr = CPP_VLA(MBufRawPacket*, arrLength);
	MBufRawPacket** allocated = CPP_VLA(MBufRawPacket*, arrLength);
	uint16_t allocatedCount = 0, packetsSent = 0, pos = 0;
	MBufRawPacket* rawPacket;

	for (RawPacketVector::VectorIterator iter = rawPacketsVec.begin(); iter != rawPacketsVec.end(); ++iter)
	{
		uint8_t raw_type = (*iter)->getObjectType();
		if (raw_type != MBUFRAWPACKET_OBJECT_TYPE)
		{
			MBufRawPacket* pkt = new MBufRawPacket();
			if (unlikely(!pkt->initFromRawPacket(*iter, this)))
			{
				delete pkt;
				goto error_out;
			}
			rawPacket = allocated[allocatedCount++] = pkt;
		}
		else
		{
			rawPacket = (MBufRawPacket*)(*iter);
		}
		mBufRawPacketArr[pos] = rawPacket;
		mBufArray[pos] = rawPacket->getMBuf();
		++pos;
	}

	packetsSent = rte_kni_tx_burst(m_Device, mBufArray, arrLength);
	for (uint16_t i = 0; i < arrLength; ++i)
	{
		mBufRawPacketArr[i]->setFreeMbuf(i >= packetsSent);
	}

error_out:
	for (uint16_t i = 0; i < allocatedCount; ++i)
		delete allocated[i];
	return packetsSent;
}

bool KniDevice::sendPacket(RawPacket& rawPacket)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}

	struct rte_mbuf* mbuf;
	MBufRawPacket* mbufRawPacket = NULL;
	bool sent = false;
	bool wasAllocated = false;

	if (rawPacket.getObjectType() != MBUFRAWPACKET_OBJECT_TYPE)
	{
		mbufRawPacket = new MBufRawPacket();
		if (unlikely(!mbufRawPacket->initFromRawPacket(&rawPacket, this)))
		{
			delete mbufRawPacket;
			return sent;
		}
		mbuf = mbufRawPacket->getMBuf();
		wasAllocated = true;
	}
	else
	{
		mbufRawPacket = (MBufRawPacket*)(&rawPacket);
		mbuf = mbufRawPacket->getMBuf();
	}

	sent = rte_kni_tx_burst(m_Device, &mbuf, 1);
	mbufRawPacket->setFreeMbuf(!sent);
	if (wasAllocated)
		delete mbufRawPacket;

	return sent;
}

bool KniDevice::sendPacket(MBufRawPacket& rawPacket)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened", m_DeviceInfo.name);
		return 0;
	}

	struct rte_mbuf* mbuf = rawPacket.getMBuf();
	bool sent = false;

	sent = rte_kni_tx_burst(m_Device, &mbuf, 1);
	rawPacket.setFreeMbuf(!sent);

	return sent;
}

bool KniDevice::sendPacket(Packet& packet)
{
	return sendPacket(*packet.getRawPacket());
}

void* KniDevice::KniCapturing::runCapture(void* devicePointer)
{
	KniDevice* device = (KniDevice*)devicePointer;
	OnKniPacketArriveCallback callback = device->m_Capturing.callback;
	void* userCookie = device->m_Capturing.userCookie;
	struct rte_mbuf* mBufArray[MAX_BURST_SIZE];
	struct rte_kni* kni = device->m_Device;

	LOG_DEBUG("Starting KNI capture thread for device \"%s\"", device->m_DeviceInfo.name);

	for(;;)
	{
		uint32_t numOfPktsReceived = rte_kni_rx_burst(kni, mBufArray, MAX_BURST_SIZE);
		if (unlikely(numOfPktsReceived == 0))
		{
			pthread_testcancel();
			continue;
		}

		timeval time;
		gettimeofday(&time, NULL);

		if (likely(callback != NULL))
		{
			MBufRawPacket rawPackets[MAX_BURST_SIZE];
			for (uint32_t index = 0; index < numOfPktsReceived; ++index)
			{
				rawPackets[index].setMBuf(mBufArray[index], time);
			}

			if (!callback(rawPackets, numOfPktsReceived, device, userCookie))
				break;
		}
		pthread_testcancel();
	}
	return NULL;
}

void KniDevice::KniCapturing::cleanup()
{
	if (thread)
		thread->cancel();
	delete thread;
	thread = NULL;
	callback = NULL;
	userCookie = NULL;
}

bool KniDevice::startCapture(
	OnKniPacketArriveCallback onPacketArrives,
	void* onPacketArrivesUserCookie
)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened. Can't start capture", m_DeviceInfo.name);
		return false;
	}
	if (unlikely(m_Capturing.thread != NULL))
	{
		LOG_ERROR("KNI device \"%s\" is already capturing", m_DeviceInfo.name);
		return false;
	}

	m_Capturing.callback = onPacketArrives;
	m_Capturing.userCookie = onPacketArrivesUserCookie;

	m_Capturing.thread = new KniThread(KniThread::JOINABLE, KniCapturing::runCapture, (void*)this);
	if (m_Capturing.thread->m_State == KniThread::INVALID)
	{
		LOG_DEBUG("KNI failed to start capturing thread on device \"%s\"",  m_DeviceInfo.name);
		delete m_Capturing.thread;
		return false;
	}

	return true;
}

void KniDevice::stopCapture()
{
	if (m_Capturing.thread == NULL)
	{
		LOG_DEBUG("Attempt to stop not running KNI capturing thread for device \"%s\"", m_DeviceInfo.name);
		return;
	}
	m_Capturing.cleanup();
}

int KniDevice::startCaptureBlockingMode(
	OnKniPacketArriveCallback onPacketArrives,
	void* onPacketArrivesUserCookie,
	int timeout
)
{
	if (unlikely(!m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is not opened. Can't start capture", m_DeviceInfo.name);
		return 0;
	}
	if (unlikely(m_Capturing.thread != NULL))
	{
		LOG_ERROR("KNI device \"%s\" is already capturing", m_DeviceInfo.name);
		return 0;
	}
	m_Capturing.callback = onPacketArrives;
	m_Capturing.userCookie = onPacketArrivesUserCookie;
	if (unlikely(m_Capturing.callback == NULL))
	{
		LOG_ERROR("Attempt to start KNI device \"%s\" capturing in blocking mode without callback", m_DeviceInfo.name);
		return 0;
	}

	struct rte_mbuf* mBufArray[MAX_BURST_SIZE];

	if (timeout <= 0)
	{
		for(;;)
		{
			uint32_t numOfPktsReceived = rte_kni_rx_burst(m_Device, mBufArray, MAX_BURST_SIZE);
			if (likely(numOfPktsReceived != 0))
			{
				MBufRawPacket rawPackets[MAX_BURST_SIZE];
				timeval time;
				gettimeofday(&time, NULL);

				for (uint32_t index = 0; index < numOfPktsReceived; ++index)
				{
					rawPackets[index].setMBuf(mBufArray[index], time);
				}

				if (!m_Capturing.callback(rawPackets, numOfPktsReceived, this, m_Capturing.userCookie))
					return 1;
			}
		}
	}
	else
	{
		long startTimeSec = 0, startTimeNSec = 0;
		long curTimeSec = 0, curTimeNSec = 0;
		clockGetTime(startTimeSec, startTimeNSec);

		while(curTimeSec <= (startTimeSec + timeout))
		{
			clockGetTime(curTimeSec, curTimeNSec);
			uint32_t numOfPktsReceived = rte_kni_rx_burst(m_Device, mBufArray, MAX_BURST_SIZE);
			if (likely(numOfPktsReceived != 0))
			{
				MBufRawPacket rawPackets[MAX_BURST_SIZE];
				timeval time;
				time.tv_sec = curTimeSec;
				time.tv_usec = curTimeNSec / 1000;

				for (uint32_t index = 0; index < numOfPktsReceived; ++index)
				{
					rawPackets[index].setMBuf(mBufArray[index], time);
				}

				if (!m_Capturing.callback(rawPackets, numOfPktsReceived, this, m_Capturing.userCookie))
					return 1;
			}
		}
	}
	return -1;
}

bool KniDevice::open()
{
	if (unlikely(m_DeviceOpened))
	{
		LOG_ERROR("KNI device \"%s\" is already opened", m_DeviceInfo.name);
		return false;
	}
	(void) updateLinkState(LINK_UP);
	switch (m_DeviceInfo.link)
	{
		case LINK_ERROR:
			return m_DeviceOpened = false;
		case LINK_NOT_SUPPORTED:
			/* fall through */
		case LINK_DOWN:
			/* fall through */
		case LINK_UP:
			return m_DeviceOpened = true;
	}
	return false;
}

void KniDevice::close()
{
	if (m_Capturing.thread != NULL)
	{
		m_Capturing.cleanup();
	}
	updateLinkState(LINK_DOWN);
	m_DeviceOpened = false;
}
} // namespace pcpp
#endif /* defined(USE_DPDK) && defined(LINUX) */