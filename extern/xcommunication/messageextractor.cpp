/*	Copyright (c) 2003-2017 Xsens Technologies B.V. or subsidiaries worldwide.
	All rights reserved.

	Redistribution and use in source and binary forms, with or without modification,
	are permitted provided that the following conditions are met:

	1.	Redistributions of source code must retain the above copyright notice,
		this list of conditions and the following disclaimer.

	2.	Redistributions in binary form must reproduce the above copyright notice,
		this list of conditions and the following disclaimer in the documentation
		and/or other materials provided with the distribution.

	3.	Neither the names of the copyright holders nor the names of their contributors
		may be used to endorse or promote products derived from this software without
		specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
	EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
	MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
	THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
	SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
	OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
	HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY OR
	TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
	SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "messageextractor.h"
#include "xcommunicationconfig.h"
#include <xsens/xsmessagearray.h>

/*! \class MessageExtractor

	Helper class that extracts XsMessages from a stream of data. The user must call the \a processNewData function every time a new block of data is available.
	It is advised not to process too small blocks of data (e.g. per byte); Every single message must not span more than \a m_maxIncompleteRetryCount blocks
	to guarantee correct operation.

	 A MessageExtractor object maintains a buffer representing a sliding window over the data stream that is just big enough to contain any incompletely received
	 XsMessage. The user can explicitly clear this buffer using the \a clearBuffer function
*/

const int MessageExtractor::m_maxIncompleteRetryCount = 5; //!< The maximum number of process attempts before advancing over an incompletely received message


/*! \brief Constructor
	\param protocolManager: the protocol manager to use for finding messages in the buffered data
*/
MessageExtractor::MessageExtractor(std::shared_ptr<IProtocolManager> protocolManager)
	: m_protocolManager { protocolManager }
	, m_retryTimeout {0}
	, m_buffer {}
{
}

/*! \brief Processes new incoming data for message extraction

	\param newData: Buffer that contains the newly arrived data
	\param messages: Newly extracted messages are stored in this vector. This vector will be cleared upon function entry
	\returns XRV_OK if one or more messages were successfully extracted. Something else if not
*/
XsResultValue MessageExtractor::processNewData(XsByteArray const& newData, std::deque<XsMessage> &messages)
{
	if (!m_protocolManager)
		return XRV_ERROR;

#ifdef XSENS_DEBUG
	XsSize prevSize = m_buffer.size();
#endif
	if (newData.size())
		m_buffer.append(newData);
#ifdef XSENS_DEBUG
	assert(m_buffer.size() == newData.size() + prevSize);
#endif

	int popped = 0;
	messages.clear();

	auto retval = [&]() {
		if (popped > 0)
			m_buffer.pop_front(popped);
		if (messages.empty())
			return XRV_TIMEOUTNODATA;

		return XRV_OK;
	};

	for (;;)
	{
		XsByteArray raw(m_buffer.data() + popped, m_buffer.size() - popped);
		XsMessage message;

		MessageLocation location = m_protocolManager->findMessage(message, raw);

		JLTRACEG("location valid: " << std::boolalpha << location.isValid() << ", looks sane: " << m_protocolManager->validateMessage(message));

		if (location.isValid() && m_protocolManager->validateMessage(message))
		{
			assert(location.m_startPos == -1 || location.m_incompletePos == -1 || location.m_incompletePos < location.m_startPos);

			if (location.m_startPos > 0)
			{
				// We are going to skip something
				if (location.m_incompletePos != -1)
				{
					// We are going to skip an incomplete but potentially valid message
					// First wait a couple of times to see if we can complete that message before skipping
					if (m_retryTimeout++ < m_maxIncompleteRetryCount)
					{
						// wait a bit until we have more data
						// but already pop the data that we know contains nothing useful
						if (location.m_incompletePos > 0)
						{
							JLALERTG("Skipping " << location.m_incompletePos << " bytes from the input buffer");
							popped += location.m_incompletePos;
						}
						return retval();
					}
					else
					{
						// We've waited a bit for the incomplete message to complete but it never completed
						// So: We are going to skip an incomplete but potentially valid message
						JLALERTG("Skipping " << location.m_startPos << " bytes from the input buffer that may contain an incomplete message at " << location.m_incompletePos
							<< " found: " << (int)message.getTotalMessageSize()
							<< std::hex << std::setfill('0')
							<< " First bytes " << std::setw(2) << (int)message.getMessageStart()[0]
							<< " " << std::setw(2) << (int)message.getMessageStart()[1]
							<< " " << std::setw(2) << (int)message.getMessageStart()[2]
							<< " " << std::setw(2) << (int)message.getMessageStart()[3]
							<< " " << std::setw(2) << (int)message.getMessageStart()[4]
							<< std::dec << std::setfill(' '));
					}
				}
				else
				{
					// We are going to skip something but we are not going to skip an incomplete but potentially valid message
					JLALERTG("Skipping " << location.m_startPos << " bytes from the input buffer");
				}
			}
			if (m_retryTimeout)
			{
				JLTRACEG("Resetting retry count from " << m_retryTimeout);
				m_retryTimeout = 0;
			}

			// message is valid, remove data from cache
			popped += location.m_size + location.m_startPos;
			messages.push_back(message);
		}
		else
		{
			return retval();
		}
	}
}

/*! \brief Clears the processing buffer
*/
void MessageExtractor::clearBuffer()
{
	m_buffer.clear();
}
