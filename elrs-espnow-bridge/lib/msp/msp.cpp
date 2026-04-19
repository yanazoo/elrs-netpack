#include "msp.h"
#include "crc.h"
#include <Arduino.h>

static uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a)
{
    static GENERIC_CRC8 crc8_dvb_s2_instance(0xD5);
    return crc8_dvb_s2_instance.calc(crc ^ a);
}

MSP::MSP() : m_inputState(MSP_IDLE) {}

bool MSP::processReceivedByte(uint8_t c)
{
    switch (m_inputState)
    {
    case MSP_IDLE:
        if (c == '$') m_inputState = MSP_HEADER_START;
        break;
    case MSP_HEADER_START:
        if (c == 'X') m_inputState = MSP_HEADER_X;
        else          m_inputState = MSP_IDLE;
        break;
    case MSP_HEADER_X:
        m_inputState = MSP_HEADER_V2_NATIVE;
        m_packet.reset();
        m_offset = 0;
        m_crc = 0;
        if      (c == '<') m_packet.type = MSP_PACKET_COMMAND;
        else if (c == '>') m_packet.type = MSP_PACKET_RESPONSE;
        else { m_packet.type = MSP_PACKET_UNKNOWN; m_inputState = MSP_IDLE; }
        break;
    case MSP_HEADER_V2_NATIVE:
        m_inputBuffer[m_offset++] = c;
        m_crc = crc8_dvb_s2(m_crc, c);
        if (m_offset == sizeof(mspHeaderV2_t))
        {
            mspHeaderV2_t *header = (mspHeaderV2_t *)&m_inputBuffer[0];
            m_packet.payloadSize = header->payloadSize;
            m_packet.function    = header->function;
            m_packet.flags       = header->flags;
            m_offset = 0;
            m_inputState = (m_packet.payloadSize == 0) ? MSP_CHECKSUM_V2_NATIVE
                                                        : MSP_PAYLOAD_V2_NATIVE;
        }
        break;
    case MSP_PAYLOAD_V2_NATIVE:
        m_packet.payload[m_offset++] = c;
        m_crc = crc8_dvb_s2(m_crc, c);
        if (m_offset == m_packet.payloadSize) m_inputState = MSP_CHECKSUM_V2_NATIVE;
        break;
    case MSP_CHECKSUM_V2_NATIVE:
        if (m_crc == c) m_inputState = MSP_COMMAND_RECEIVED;
        else { Serial.printf("[msp] CRC error: got %d expected %d\n", c, m_crc); m_inputState = MSP_IDLE; }
        break;
    default:
        m_inputState = MSP_IDLE;
        break;
    }
    return (m_inputState == MSP_COMMAND_RECEIVED);
}

mspPacket_t *MSP::getReceivedPacket() { return &m_packet; }

void MSP::markPacketReceived() { m_inputState = MSP_IDLE; }

uint8_t MSP::convertToByteArray(mspPacket_t *packet, uint8_t *byteArray)
{
    if (packet->type != MSP_PACKET_COMMAND && packet->type != MSP_PACKET_RESPONSE) return 0;
    if (packet->type == MSP_PACKET_RESPONSE && packet->payloadSize == 0) return 0;

    uint8_t pos = 0;
    byteArray[pos++] = '$';
    byteArray[pos++] = 'X';
    byteArray[pos++] = (packet->type == MSP_PACKET_COMMAND) ? '<' : '>';

    uint8_t crc = 0;
    uint8_t headerBuffer[sizeof(mspHeaderV2_t)];
    mspHeaderV2_t *header = (mspHeaderV2_t *)headerBuffer;
    header->flags       = packet->flags;
    header->function    = packet->function;
    header->payloadSize = packet->payloadSize;

    for (uint8_t i = 0; i < sizeof(mspHeaderV2_t); i++)
    { byteArray[pos++] = headerBuffer[i]; crc = crc8_dvb_s2(crc, headerBuffer[i]); }

    for (uint16_t i = 0; i < packet->payloadSize; i++)
    { byteArray[pos++] = packet->payload[i]; crc = crc8_dvb_s2(crc, packet->payload[i]); }

    byteArray[pos++] = crc;
    return pos;
}

uint8_t MSP::getTotalPacketSize(mspPacket_t *packet)
{
    uint8_t size = 3; // $ X type
    size += sizeof(mspHeaderV2_t);
    size += packet->payloadSize;
    size += 1; // crc
    return size;
}
