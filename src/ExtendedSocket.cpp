#include "ExtendedSocket.h"
#include "SendPacket.h"

#include <wolfssl/options.h>
#include <wolfssl/openssl/evp.h>

using namespace std;

CExtendedSocket::CExtendedSocket(unsigned int id)
{
	memset(&m_GuestData, 0, sizeof(GuestData_s));
	m_nID = id;
	m_Socket = 0;
	m_nSequence = 0;
	m_nBytesReceived = 0;
	m_nBytesSent = 0;
	m_nPacketReceivedSize = 0;
	m_nPacketToReceiveFullSize = 0;
	m_nReadResult = 0;
	m_nNextExpectedSeq = 1;
	m_pMsg = NULL;
	m_pEncEVPCTX = NULL;
	m_pDecEVPCTX = NULL;
	m_bCryptInput = false;
	m_bCryptOutput = false;
}

CExtendedSocket::~CExtendedSocket()
{
	if (m_pMsg)
		delete m_pMsg;

	for (auto msg : m_SendPackets)
		delete msg;

	if (m_pDecEVPCTX)
	{
		EVP_CIPHER_CTX_cleanup(m_pDecEVPCTX);
		EVP_CIPHER_CTX_free(m_pDecEVPCTX);
	}

	if (m_pEncEVPCTX)
	{
		EVP_CIPHER_CTX_cleanup(m_pEncEVPCTX);
		EVP_CIPHER_CTX_free(m_pEncEVPCTX);
	}
}

// Setup packet cipher and create key
bool CExtendedSocket::SetupCrypt()
{
	if (m_pEncEVPCTX && m_pDecEVPCTX)
	{
		return true;
	}

	memset(m_pCryptKey, 0, 64);
	memset(m_pCryptIV, 0, 64);

	int keyLen = EVP_BytesToKey(EVP_aes_128_cbc(), EVP_md5(), NULL, m_HWID.data(), m_HWID.size(), 1, m_pCryptKey, m_pCryptIV);
	if (!keyLen)
	{
		return false;
	}

	m_pEncEVPCTX = EVP_CIPHER_CTX_new();
	m_pDecEVPCTX = EVP_CIPHER_CTX_new();

	EVP_CipherInit(m_pEncEVPCTX, EVP_rc4(), NULL, NULL, 1);
	EVP_CipherInit(m_pDecEVPCTX, EVP_rc4(), NULL, NULL, 0);

	// useless?
	//EVP_CIPHER_CTX_set_padding(m_pEncEVPCTX, 0);
	//EVP_CIPHER_CTX_set_padding(m_pDecEVPCTX, 0);

	if (EVP_CipherInit(m_pEncEVPCTX, EVP_rc4(), (const unsigned char*)m_pCryptKey, (const unsigned char*)m_pCryptIV, 1) != 1)
	{
		EVP_CIPHER_CTX_cleanup(m_pDecEVPCTX);
		EVP_CIPHER_CTX_cleanup(m_pEncEVPCTX);

		EVP_CIPHER_CTX_free(m_pDecEVPCTX);
		EVP_CIPHER_CTX_free(m_pEncEVPCTX);

		m_pDecEVPCTX = NULL;
		m_pEncEVPCTX = NULL;

		return false;
	}

	if (EVP_CipherInit(m_pDecEVPCTX, EVP_rc4(), (const unsigned char*)m_pCryptKey, (const unsigned char*)m_pCryptIV, 0) != 1)
	{
		EVP_CIPHER_CTX_cleanup(m_pDecEVPCTX);
		EVP_CIPHER_CTX_cleanup(m_pEncEVPCTX);

		EVP_CIPHER_CTX_free(m_pDecEVPCTX);
		EVP_CIPHER_CTX_free(m_pEncEVPCTX);

		m_pDecEVPCTX = NULL;
		m_pEncEVPCTX = NULL;

		return false;
	}

	return true;
}

int CExtendedSocket::GetSeq()
{
	if (m_nSequence > MAX_SEQUENCE)
	{
		ResetSeq();
	}

	return m_nSequence++;
}

int CExtendedSocket::LoggerGetSeq()
{
	return m_nSequence;
}

void CExtendedSocket::ResetSeq()
{
	m_nSequence = 0;
}

CReceivePacket* CExtendedSocket::Read()
{
	m_nReadResult = 0;
	vector<unsigned char> packetDataBuf;
	packetDataBuf.resize(PACKET_HEADER_SIZE);
	int recvResult = 0;

	if (!m_pMsg)
	{
		// first of all read the packet header to know is received packet is valid
		recvResult = g_pNetwork->ReceiveMessage(m_Socket, (char*)packetDataBuf.data(), PACKET_HEADER_SIZE);
		if (recvResult < 4)
		{
			g_pConsole->Warn("CExtendedSocket::Read(%s): result < PACKET_HEADER_SIZE, %d\n", GetIP().c_str(), WSAGetLastError());
			return NULL;
		}

		// update bytes counters
		m_nReadResult += recvResult; 
		m_nBytesReceived += recvResult;
		if (m_nBytesReceived < 0)
			m_nBytesReceived = 0;

		// decrypt header if encrypted
		if (m_bCryptInput)
		{
			int outLen = 0;
			if (EVP_DecryptUpdate(m_pDecEVPCTX, packetDataBuf.data(), &outLen, packetDataBuf.data(), recvResult) != 1)
			{
				g_pConsole->Log("CExtendedSocket::Read: EVP_DecryptUpdate failed\n");
			}

			int finalLen = 0;
			if (EVP_DecryptFinal_ex(m_pDecEVPCTX, packetDataBuf.data() + outLen, &finalLen) != 1)
			{
				g_pConsole->Log("CExtendedSocket::Read: EVP_DecryptUpdate failed\n");
			}
		}

		// when a people may incorrect once packet data, might spammed this message forever....
		m_pMsg = new CReceivePacket(Buffer(packetDataBuf));
		if (!m_pMsg->IsValid())
		{
			g_pConsole->Error("CExtendedSocket::Read(%s): received invalid packet\n", GetIP().c_str());
			delete m_pMsg;
			m_pMsg = NULL;
			return NULL;
		}

		if (m_pMsg->GetSequence() != m_nNextExpectedSeq)
		{
			g_pConsole->Warn("CExtendedSocket::Read(%s): sequence mismatch, got: %d, expected: %d\n", GetIP().c_str(), m_pMsg->GetSequence(), m_nNextExpectedSeq);
			//return NULL;
		}

		// well i think set to -1 afterwards, so fixed shits
		if (m_nNextExpectedSeq == 255)
			m_nNextExpectedSeq = -1;

		// here causes seq never 0 lol
		m_nNextExpectedSeq++;

		m_nPacketToReceiveFullSize = m_pMsg->GetLength();
		m_nPacketToReceiveFullSize += PACKET_HEADER_SIZE;

		m_nPacketReceivedSize = 0;
	}

	packetDataBuf.resize(m_pMsg->GetLength());
	recvResult = g_pNetwork->ReceiveMessage(m_Socket, (char*)packetDataBuf.data(), m_nPacketReceivedSize ? m_pMsg->GetLength() + PACKET_HEADER_SIZE - m_nPacketReceivedSize : m_pMsg->GetLength());
	if (recvResult <= 0)
	{
		g_pConsole->Warn("CExtendedSocket::Read(%s): result <= 0\n", GetIP().c_str(), WSAGetLastError());
		delete m_pMsg;
		m_pMsg = NULL;
		return NULL;
	}

	packetDataBuf.resize(recvResult);

	m_nReadResult += recvResult;
	m_nBytesReceived += recvResult;
	if (m_nBytesReceived < 0)
		m_nBytesReceived = 0;

	// decrypt the rest part of packet
	if (m_bCryptInput)
	{
		int outLen = 0;
		if (EVP_DecryptUpdate(m_pDecEVPCTX, packetDataBuf.data(), &outLen, packetDataBuf.data(), recvResult) != 1)
		{
			g_pConsole->Log("CExtendedSocket::Read: EVP_DecryptUpdate failed\n");
		}

		int finalLen = 0;
		if (EVP_DecryptFinal_ex(m_pDecEVPCTX, packetDataBuf.data() + outLen, &finalLen) != 1)
		{
			g_pConsole->Log("CExtendedSocket::Read: EVP_DecryptUpdate failed\n");
		}
	}

	Buffer& buf = m_pMsg->GetData();
	// todo: rewrite
	vector<unsigned char> vecBuf = buf.getBuffer();
	vecBuf.insert(vecBuf.end(), packetDataBuf.begin(), packetDataBuf.end());
	buf.setBuffer(vecBuf);

#if 0
	g_pConsole->Server->Log("CExtendedSocket::Read: recvResult: %d, packetDataBuf.size: %d, m_nPacketReceivedSize: %d, m_pMsg->GetLength: %d, m_pMsg->GetSequence: %d, m_nPacketToReceiveFullSize: %d\n", recvResult, packetDataBuf.size(), m_nPacketReceivedSize, m_pMsg->GetLength(), m_pMsg->GetSequence(), m_nPacketToReceiveFullSize);
#endif

	// if not received the full packet
	if (m_nPacketToReceiveFullSize != buf.getBuffer().size())
	{
		m_nPacketReceivedSize = buf.getBuffer().size();
		return NULL;
	}

	buf.setReadOffset(0);
	m_pMsg->ParseHeader();

	return m_pMsg;
}

int CExtendedSocket::Send(vector<unsigned char>& buffer)
{
	if (buffer.size() > PACKET_MAX_SIZE)
	{
		g_pConsole->Error("CExtendedSocket::Send() buffer.size(): %d > PACKET_MAX_SIZE!!!, ID: %d, seq: %d. Packet not sent.\n", buffer.size(), buffer[4], buffer[1]);
		m_nSequence--;
		return 0;
	}

	std::vector<unsigned char> outBuf;
	outBuf.resize(buffer.size());

	if (m_bCryptOutput)
	{
		int encLen = 0;
		if (EVP_EncryptUpdate(m_pEncEVPCTX, outBuf.data(), &encLen, buffer.data(), buffer.size()) != 1)
		{
			g_pConsole->Log("CExtendedSocket::Send: EVP_EncryptUpdate failed\n");
		}

		int finalLen = 0;
		if (EVP_EncryptFinal_ex(m_pEncEVPCTX, outBuf.data() + encLen, &finalLen) != 1)
		{
			g_pConsole->Log("CExtendedSocket::Read: EVP_EncryptUpdate failed\n");
		}

		if (encLen != buffer.size())
		{
			g_pConsole->Log("CExtendedSocket::Send: encLen != buffer.size()\n");
		}
	}

	outBuf = buffer;

	const char* bufData = reinterpret_cast<const char*>(&outBuf[0]);

	int bytesSent = g_pNetwork->SendMessage(m_Socket, bufData, buffer.size());
	if (bytesSent != buffer.size())
		return bytesSent;

	m_nBytesSent += bytesSent;
	if (m_nBytesSent < 0)
		m_nBytesSent = 0;

#ifdef _DEBUG
	g_pConsole->Log("CExtendedSocket::Send() seq: %d, buffer.size(): %d, bytesSent: %d, id: %d\n", buffer[1], buffer.size(), bytesSent, buffer[4]);
#endif

	return bytesSent;
}

// complicated shit
int CExtendedSocket::Send(CSendPacket* msg, bool forceSend)
{
	int result = 1;

	if (!forceSend && m_SendPackets.size())
	{
		m_SendPackets.push_back(msg);
	}
	else
	{
		auto data = msg->SetPacketLength();
		result = Send(data);
		int error = WSAGetLastError();
		if (result != msg->GetData().getBuffer().size() && error)
		{
			if (error == WSAEWOULDBLOCK)
			{
				m_SendPackets.push_back(msg);
			}
			else
			{
				g_pConsole->Error("CExtendedSocket::Send() WSAGetLastError: %d\n", error);
			}
		}
		else
		{
			delete msg;
		}
	}

	return result;
}

int CExtendedSocket::GetID()
{
	return m_nID;
}

void CExtendedSocket::SetSocket(SOCKET socket)
{
	m_Socket = socket;
}

SOCKET CExtendedSocket::GetSocket()
{
	return m_Socket;
}

CReceivePacket* CExtendedSocket::GetMsg()
{
	return m_pMsg;
}

void CExtendedSocket::SetMsg(CReceivePacket* msg)
{
	m_pMsg = msg;
}

int CExtendedSocket::GetReadResult()
{
	return m_nReadResult;
}

int CExtendedSocket::GetBytesReceived()
{
	return m_nBytesReceived;
}

int CExtendedSocket::GetBytesSent()
{
	return m_nBytesSent;
}

std::vector<CSendPacket*> CExtendedSocket::GetPacketsToSend()
{
	return m_SendPackets;
}

GuestData_s& CExtendedSocket::GetGuestData()
{
	return m_GuestData;
}