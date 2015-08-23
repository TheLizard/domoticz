#include "stdafx.h"
#include "proxyclient.h"
#include "request.hpp"
#include "reply.hpp"
#include "request_parser.hpp"
#include "../main/SQLHelper.h"
#include "../webserver/Base64.h"

extern std::string szAppVersion;

CProxyClient::CProxyClient(boost::asio::io_service& io_service, boost::asio::ssl::context& context, http::server::cWebem *webEm)
    : _socket(io_service, context),
		_io_service(io_service),
	  doStop(false)
{
	_apikey = "";
	_instanceid = "";
	_password = "";
	m_sql.GetPreferencesVar("MyDomoticzInstanceId", _instanceid);
	m_sql.GetPreferencesVar("MyDomoticzUserId", _apikey);
	m_sql.GetPreferencesVar("MyDomoticzPassword", _password);
	if (_password != "") {
		_password = base64_decode(_password);
	}
	if (_apikey == "" || _password == "") {
		doStop = true;
		return;
	}
	_log.Log(LOG_NORM, "PROXY: Connecting.");
	m_pWebEm = webEm;
	Reconnect();
}

void CProxyClient::Reconnect()
{
	_log.Log(LOG_NORM, "PROXY: Connecting...");

	std::string address = "my.domoticz.com";
	std::string port = "9999";

	boost::asio::ip::tcp::resolver resolver(_io_service);
	boost::asio::ip::tcp::resolver::query query(address, port);
	boost::asio::ip::tcp::resolver::iterator iterator = resolver.resolve(query);
	boost::asio::ip::tcp::endpoint endpoint = *iterator;
	_log.Log(LOG_NORM, "PROXY: we have an iterator");
	_socket.lowest_layer().async_connect(endpoint,
		boost::bind(&CProxyClient::handle_connect, this,
		boost::asio::placeholders::error, iterator));
}

void CProxyClient::handle_connect(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
{
	_log.Log(LOG_NORM, "PROXY: In handle_connect.");
	if (!error)
	{
		_log.Log(LOG_NORM, "PROXY: Connect complete. Starting handshake.");
		_socket.async_handshake(boost::asio::ssl::stream_base::client,
			boost::bind(&CProxyClient::handle_handshake, this,
			boost::asio::placeholders::error));
	}
	else if (endpoint_iterator != boost::asio::ip::tcp::resolver::iterator())
	{
		_log.Log(LOG_NORM, "PROXY: Trying next endpoint iterator.");
		_socket.lowest_layer().close();
		boost::asio::ip::tcp::endpoint endpoint = *endpoint_iterator;
		_socket.lowest_layer().async_connect(endpoint,
			boost::bind(&CProxyClient::handle_connect, this,
			boost::asio::placeholders::error, ++endpoint_iterator));
	}
	else
	{
		_log.Log(LOG_ERROR, "PROXY: Connect failed, error %d. Reconnecting", error);
		if (!doStop) {
			boost::this_thread::sleep_for(boost::chrono::seconds(10));
			Reconnect();
		}
	}
}

void CProxyClient::MyWrite(pdu_type type, CValueLengthPart *parameters)
{
	_writebuf.clear();
	writePdu = new ProxyPdu(type, parameters);
	_log.Log(LOG_NORM, "PROXY: Writing pdu.");

	_writebuf.push_back(boost::asio::buffer(writePdu->content(), writePdu->length()));

	boost::asio::async_write(_socket, _writebuf,
		boost::bind(&CProxyClient::handle_write, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred));
}

void CProxyClient::LoginToService()
{
	// send authenticate pdu
	CValueLengthPart parameters;
	parameters.AddPart((void *)_apikey.c_str(), _apikey.length() + 1);
	parameters.AddPart((void *)_instanceid.c_str(), _instanceid.length() + 1);
	parameters.AddPart((void *)_password.c_str(), _password.length() + 1);
	parameters.AddPart((void *) szAppVersion.c_str(), szAppVersion.length() + 1);
	// todo: version
	MyWrite(PDU_AUTHENTICATE, &parameters);
}

void CProxyClient::handle_handshake(const boost::system::error_code& error)
{
	if (!error)
	{
		_log.Log(LOG_NORM, "PROXY: Handshake complete. Authenticating.");
		LoginToService();
	}
	else
	{
		_log.Log(LOG_ERROR, "PROXY: Handshake failed, error %d. Reconnecting.", error);
		Reconnect();
	}
}

void CProxyClient::ReadMore()
{
	// read chunks of max 4 KB
	boost::asio::streambuf::mutable_buffers_type buf = _readbuf.prepare(4096);
	_socket.async_read_some(buf,
		boost::bind(&CProxyClient::handle_read, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
	);
}

void CProxyClient::handle_write(const boost::system::error_code& error, size_t bytes_transferred)
{
	delete writePdu;
	if (!error)
	{
		_log.Log(LOG_NORM, "PROXY: Write complete. Reading pdu.");
		ReadMore();
	}
	else
	{
		_log.Log(LOG_ERROR, "PROXY: Write failed, error %d", error);
	}
}

void CProxyClient::GetRequest(const std::string originatingip, boost::asio::mutable_buffers_1 _buf, http::server::reply &reply_)
{
	/// The parser for the incoming request.
	http::server::request_parser request_parser_;
	http::server::request request_;

	boost::tribool result;
	try
	{
		size_t bufsize = boost::asio::buffer_size(_buf);
		const char *begin = boost::asio::buffer_cast<const char*>(_buf);
		boost::tie(result, boost::tuples::ignore) = request_parser_.parse(
			request_, begin, begin + bufsize);
	}
	catch (...)
	{
		_log.Log(LOG_ERROR, "PROXY: Exception during request parsing");
	}

	if (result)
	{
			m_pWebEm->myRequestHandler.handle_request(originatingip, request_, reply_);
			//std::vector<boost::asio::const_buffer> replybuf = reply_.to_buffers();
			//response = std::string(boost::asio::buffers_begin(replybuf), boost::asio::buffers_begin(replybuf) + boost::asio::buffer_size(replybuf));
			_log.Log(LOG_ERROR, "PROXY: We have a response");
	}
	else if (!result)
	{
		_log.Log(LOG_ERROR, "PROXY: Parse result: false.");
		reply_ = http::server::reply::stock_reply(http::server::reply::bad_request);
		//std::vector<boost::asio::const_buffer> replybuf = reply_.to_buffers();
		//response = std::string(boost::asio::buffers_begin(replybuf), boost::asio::buffers_begin(replybuf) + boost::asio::buffer_size(replybuf));
	}
	else
	{
		_log.Log(LOG_ERROR, "Could not parse request.");
	}
}

std::string CProxyClient::GetResponseHeaders(const http::server::reply &reply_)
{
	std::string result = "";
	for (std::size_t i = 0; i < reply_.headers.size(); ++i) {
		result += reply_.headers[i].name;
		result += ": ";
		result += reply_.headers[i].value;
		result += "\r\n";
	}
	return result;
}

void CProxyClient::HandleRequest(ProxyPdu *pdu)
{
	// response variables
	boost::asio::mutable_buffers_1 _buf(NULL, 0);
	/// The reply to be sent back to the client.
	http::server::reply reply_;

	// get url from request
	CValueLengthPart part(pdu);
	char *originatingip;
	int subsystem;
	char *requesturl;
	char *requestheaders;
	char *requestbody;
	size_t thelen, bodylen;
	part.GetNextPart((void **)&originatingip, &thelen);
	_log.Log(LOG_NORM, "PROXY: Originating ip %s.", originatingip);
	part.GetNextValue((void **)&subsystem, &thelen);
	_log.Log(LOG_NORM, "PROXY: Subsystem %d.", subsystem);
	part.GetNextPart((void **)&requesturl, &thelen);
	_log.Log(LOG_NORM, "PROXY: This is a request pdu for %s.", requesturl);
	part.GetNextPart((void **)&requestheaders, &thelen);
	part.GetNextPart((void **)&requestbody, &bodylen);
	_log.Log(LOG_NORM, "PROXY: Body len: %ld.", bodylen);
			
	CValueLengthPart parameters;

	switch (subsystem) {
	case 1:
		// "normal web request"
		std::string request;
		if (bodylen > 0) {
			request = "POST ";
		}
		else {
			request = "GET ";
		}
		request += requesturl;
		request += " HTTP/1.1\r\n";
		request += requestheaders;
		request += "\r\n";
		request += std::string(requestbody, bodylen);

		_buf = boost::asio::buffer((void *)request.c_str(), request.length());

		GetRequest(originatingip, _buf, reply_);
		free(originatingip);
		free(requesturl);
		free(requestheaders);
		free(requestbody);

		// assemble response
		std::string responseheaders = GetResponseHeaders(reply_);


		_log.Log(LOG_NORM, "PROXY: Response, status: %d.", reply_.status);
		_log.Log(LOG_NORM, "PROXY: Response length: %ld.", reply_.content.length());
		parameters.AddValue((void *)&reply_.status, SIZE_INT);
		parameters.AddPart((void *)responseheaders.c_str(), responseheaders.length() + 1);
		parameters.AddPart((void *)reply_.content.c_str(), reply_.content.length());
		_log.Log(LOG_NORM, "About to send PDU.\n");
		break;
	}

	// send response to proxy
	MyWrite(PDU_RESPONSE, &parameters);
}

void CProxyClient::HandleAssignkey(ProxyPdu *pdu)
{
	std::string old_id;
	// get our new api key
	CValueLengthPart parameters(pdu);
	char *newapi;
	size_t newapilen;
	parameters.GetNextPart((void **)&newapi, &newapilen);
	_log.Log(LOG_NORM, "PROXY: We were assigned an instance id: %s.\n", newapi);
	prefs_mutex.lock();
	m_sql.GetPreferencesVar("MyDomoticzInstanceId", old_id);
	if (old_id == "") {
		// check if another thread meanwhile also didn't get an instance id
		_instanceid = newapi;
		m_sql.UpdatePreferencesVar("MyDomoticzInstanceId", _instanceid);
	}
	prefs_mutex.unlock();
	free(newapi);
	// re-login with the new instance id
	LoginToService();
}

void CProxyClient::HandleEnquire(ProxyPdu *pdu)
{
	_log.Log(LOG_NORM, "PROXY: Receiving enquire.");

	// assemble response
	CValueLengthPart parameters;

	// send response to proxy
	MyWrite(PDU_ENQUIRE, &parameters);
}

void CProxyClient::HandleAuthresp(ProxyPdu *pdu)
{
	// get auth response (0 or 1)
	size_t authlen;
	int auth;
	CValueLengthPart part(pdu);
	part.GetNextValue((void **)&auth, &authlen);
	_log.Log(LOG_NORM, "PROXY: Authenticated: %s (%d).\n", auth ? "yes" : "no", auth);
	if (!auth) {
		Stop();
		return;
	}
	ReadMore();
}

void CProxyClient::handle_read(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (!error)
	{
		_log.Log(LOG_ERROR, "PROXY: Reply: %ld bytes transferred.", bytes_transferred);
		
		_readbuf.commit(bytes_transferred);
		const char *data = boost::asio::buffer_cast<const char*>(_readbuf.data());
		ProxyPdu pdu(data, _readbuf.size());
		if (pdu.Disconnected()) {
			ReadMore();
			return;
		}
		_log.Log(LOG_NORM, "Consuming %d bytes.", pdu.length() + 9);
		_readbuf.consume(pdu.length() + 9); // 9 is header size

		switch (pdu._type) {
		case PDU_REQUEST:
			HandleRequest(&pdu);
			break;
		case PDU_ASSIGNKEY:
			HandleAssignkey(&pdu);
			break;
		case PDU_ENQUIRE:
			HandleEnquire(&pdu);
			break;
		case PDU_AUTHRESP:
			HandleAuthresp(&pdu);
			break;
		default:
			_log.Log(LOG_ERROR, "PROXY: pdu type: %d not expected.", pdu._type);
			ReadMore();
			break;
		}
	}
	else
	{
		_log.Log(LOG_ERROR, "PROXY: Read failed, error %d, reconnecting.", error);
		// Initiate graceful connection closure.
		_socket.lowest_layer().close();
		if (doStop) {
			return;
		}
		// we are disconnected, reconnect
		boost::this_thread::sleep_for(boost::chrono::seconds(10));
		Reconnect();
	}
}


void CProxyClient::Stop()
{
	doStop = true;
	_socket.lowest_layer().close();
}

CProxyClient::~CProxyClient()
{
}

CProxyManager::CProxyManager(const std::string& doc_root, http::server::cWebem *webEm)
{
	proxyclient = NULL;
	m_pWebEm = webEm;
}

CProxyManager::~CProxyManager()
{
	//end_mutex.lock();
	// todo: throws access violation
	if (proxyclient) delete proxyclient;
}

int CProxyManager::Start()
{
	end_mutex.lock();
	m_thread = new boost::thread(boost::bind(&CProxyManager::StartThread, this));
	return 1;
}

void CProxyManager::StartThread()
{
	try {
		boost::asio::ssl::context ctx(io_service, boost::asio::ssl::context::sslv23);
#if 0	
		ctx.set_verify_mode(boost::asio::ssl::context::verify_peer);
		ctx.load_verify_file("ca.pem");
#else
		ctx.set_verify_mode(boost::asio::ssl::verify_none);
#endif

		proxyclient = new CProxyClient(io_service, ctx, m_pWebEm);

		io_service.run();
	}
	catch (std::exception& e)
	{
		_log.Log(LOG_ERROR, "PROXY: StartThread(): Exception: %s", e.what());
	}
}

void CProxyManager::Stop()
{
	proxyclient->Stop();
	io_service.stop();
	_log.Log(LOG_ERROR, "PROXY: waiting for thread.join");
	m_thread->interrupt();
	m_thread->join();
}
