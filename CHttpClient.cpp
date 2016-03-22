#include "pch.h"
#include "CHttpClient.h"

#pragma comment ( lib, "ws2_32.lib" )
#pragma comment ( lib, "wldap32.lib" )
#ifdef _DEBUG
#pragma comment(lib,"libcurl_d.lib")
#else
#pragma comment(lib,"libcurl.lib")

#endif // _DEBUG


template<> CHttpClient* Singleton<CHttpClient>::ms_Singleton = NULL;
LONG64 httpRequestUUID = 0;
CHttpClient::CHttpClient(void) : m_bDebug(false), m_atomic(0), m_pWorkThread(NULL), m_Quit(false)
{
	m_pWorkThread = new boost::thread(boost::bind(&CHttpClient::workThreadFunc, this));
}

CHttpClient::~CHttpClient(void)
{
	m_Quit = true;
	if (m_pWorkThread != NULL) {
		m_pWorkThread->join();
	}
	SafeDelete(m_pWorkThread);
}

static int OnDebug(CURL *, curl_infotype itype, char * pData, size_t size, void *)
{
	if (itype == CURLINFO_TEXT) {
		//printf("[TEXT]%s\n", pData);
	}
	else if (itype == CURLINFO_HEADER_IN) {
		printf("[HEADER_IN]%s\n", pData);
	}
	else if (itype == CURLINFO_HEADER_OUT) {
		printf("[HEADER_OUT]%s\n", pData);
	}
	else if (itype == CURLINFO_DATA_IN) {
		printf("[DATA_IN]%s\n", pData);
	}
	else if (itype == CURLINFO_DATA_OUT) {
		printf("[DATA_OUT]%s\n", pData);
	}
	return 0;
}

static size_t OnWriteData(void* buffer, size_t size, size_t nmemb, void* lpVoid)
{
	std::string* str = static_cast<std::string*> (lpVoid);
	char* pData = (char*)buffer;
	if (NULL == str || NULL == pData) {
		return -1;
	}

	str->append(pData, size * nmemb);
	return nmemb * size;
}

static size_t OnAsyncWriteData(void* buffer, size_t size, size_t nmemb, void* lpVoid)
{
	// CHttpRequestCollect::iterator* pit = static_cast<CHttpRequestCollect::iterator*> (lpVoid);
	xstring* pStr = static_cast<std::string*>(lpVoid);
	char* pData = (char*)buffer;
	if (NULL == pStr || NULL == pData) {
		return -1;
	}

	pStr->append(pData, size * nmemb);
	return nmemb * size;
}

int CHttpClient::Get(const std::string & strUrl, std::string & strResponse)
{
	CURLcode res;
	CURL* curl = curl_easy_init();
	if (NULL == curl) {
		return CURLE_FAILED_INIT;
	}
	if (m_bDebug) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, OnDebug);
	}
	curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);
	/**
	 * 当多个线程都使用超时处理的时候，同时主线程中有sleep或是wait等操作。
	 * 如果不设置这个选项，libcurl将会发信号打断这个wait从而导致程序退出。
	 */
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_connectTimeOut);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_requestTimeOut);
	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return res;
}

int CHttpClient::asyncGet(const std::string & strUrl, HttpCallBack cb)
{
	CHttpRequestPtr ptr(new CHttpRequest(strUrl, xstring(""), cb));
	boost::mutex::scoped_lock lock(m_NewRequestMutex);
	m_NewRequestCollect.push_back(ptr);
	lock.unlock();

	return 0;
}

int CHttpClient::Post(const std::string & strUrl, const std::string & strPost, std::string & strResponse)
{
	CURLcode res;
	CURL* curl = curl_easy_init();
	if (NULL == curl) {
		return CURLE_FAILED_INIT;
	}
	if (m_bDebug) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, OnDebug);
	}
	curl_easy_setopt(curl, CURLOPT_URL, strUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strPost.c_str());
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&strResponse);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_connectTimeOut);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_requestTimeOut);
	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return res;
}

LONG64 CHttpClient::asyncPost(const std::string & strUrl, const std::string & strPost, HttpCallBack cb)
{
	CHttpRequestPtr ptr(new CHttpRequest(strUrl, strPost, cb));
	boost::mutex::scoped_lock lock(m_NewRequestMutex);
	m_NewRequestCollect.push_back(ptr);
	lock.unlock();
	//ptr->m_cbObj = obj;
	return ptr->uuid;
}

///////////////////////////////////////////////////////////////////////////////////////////////

void CHttpClient::SetDebug(bool bDebug)
{
	m_bDebug = bDebug;
}

void CHttpClient::workThreadFunc()
{
	using namespace std;
	curl_global_init(CURL_GLOBAL_ALL);
	m_handMultiCurl = curl_multi_init();
	bool debug = false;
	while (m_Quit == false) {
		Sleep(50);
		//取出request
		// 设置easy curl对象并添加到multi curl对象中  //
		boost::mutex::scoped_lock lock(m_NewRequestMutex);
		CHttpRequestCollect tmp = m_NewRequestCollect;
		m_NewRequestCollect.clear();
		lock.unlock();


		CHttpRequestCollect::iterator it = tmp.begin();
		CHttpRequestCollect::iterator itend = tmp.end();
		for (; it != itend; ++it) {
			CURL* hUrl = curl_easy_handler((*it)->m_url, (*it)->m_postData, it);
			if (hUrl == NULL) {
				continue;
			}
			m_DoingRequestCollect.insert(std::make_pair((LONG64)hUrl, *it));

			curl_multi_add_handle(m_handMultiCurl, hUrl);

		}
		if (debug)
			cout << "add handler finish" << endl;
		/*
		 * 调用curl_multi_perform函数执行curl请求
		 * url_multi_perform返回CURLM_CALL_MULTI_PERFORM时，表示需要继续调用该函数直到返回值不是CURLM_CALL_MULTI_PERFORM为止
		 * running_handles变量返回正在处理的easy curl数量，running_handles为0表示当前没有正在执行的curl请求
		 */
		int running_handles;
		while (CURLM_CALL_MULTI_PERFORM == curl_multi_perform(m_handMultiCurl, &running_handles)) {
			//cout << running_handles << endl;
		}
		if (debug)
			cout << "exec request finish" << endl;
		/**
		 * 为了避免循环调用curl_multi_perform产生的cpu持续占用的问题，采用select来监听文件描述符
		 */
		//        while (running_handles)
		//        {
		if (running_handles > 0) {
			if (-1 == curl_multi_select(m_handMultiCurl)) {
				//cerr << "select error" << endl;
				continue;
			}
		}
		// select监听到事件，调用curl_multi_perform通知curl执行相应的操作 //
		//        while (CURLM_CALL_MULTI_PERFORM == curl_multi_perform(m_handMultiCurl, &running_handles));
		//        {
		//            cout << "select2: " << running_handles << endl;
		//        }
		//cout << "select: " << running_handles << endl;
		//        }

		// 输出执行结果 //
		int msgs_left = 0;
		CURLMsg * msg;
		while (msg = curl_multi_info_read(m_handMultiCurl, &msgs_left)) {

			if (CURLMSG_DONE == msg->msg) {
				//CHttpRequestCollect::iterator itd;
				//curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &itd);
				CHttpRequestMap::iterator itd = m_DoingRequestCollect.find((LONG64)msg->easy_handle);
				if (itd != m_DoingRequestCollect.end()) {

					(itd->second)->m_state = msg->data.result;
					boost::mutex::scoped_lock lock(m_FinishRequestMutex);
					m_FinishRequestCollect.push_back(itd->second);
					lock.unlock();

					m_DoingRequestCollect.erase(itd);
					curl_multi_remove_handle(m_handMultiCurl, msg->easy_handle);
					curl_easy_cleanup(msg->easy_handle);
				}

			}
			else {
				cerr << "bad request, result:" << msg->msg << endl;
			}

		}
		if (debug)
			cout << "read info finish:" << endl;

	}
	curl_multi_cleanup(m_handMultiCurl);
	curl_global_cleanup();
}

int CHttpClient::curl_multi_select(CURLM * curl_m)
{
	int ret = 0;

	struct timeval timeout_tv;
	fd_set fd_read;
	fd_set fd_write;
	fd_set fd_except;
	int max_fd = -1;

	// 注意这里一定要清空fdset,curl_multi_fdset不会执行fdset的清空操作  //
	FD_ZERO(&fd_read);
	FD_ZERO(&fd_write);
	FD_ZERO(&fd_except);

	// 设置select超时时间  //
	timeout_tv.tv_sec = 0;
	timeout_tv.tv_usec = 50000;

	// 获取multi curl需要监听的文件描述符集合 fd_set //
	CURLMcode mc = curl_multi_fdset(curl_m, &fd_read, &fd_write, &fd_except, &max_fd);

	if (mc != CURLM_OK) {
		fprintf(stderr, "curl_multi_fdset() failed, code %d.\n", mc);
		return -1;
	}

	/**
	 * When max_fd returns with -1,
	 * you need to wait a while and then proceed and call curl_multi_perform anyway.
	 * How long to wait? I would suggest 100 milliseconds at least,
	 * but you may want to test it out in your own particular conditions to find a suitable value.
	 */
	if (-1 == max_fd) {
		return -1;
	}

	/**
	 * 执行监听，当文件描述符状态发生改变的时候返回
	 * 返回0，程序调用curl_multi_perform通知curl执行相应操作
	 * 返回-1，表示select错误
	 * 注意：即使select超时也需要返回0，具体可以去官网看文档说明
	 */
	int ret_code = ::select(max_fd + 1, &fd_read, &fd_write, &fd_except, &timeout_tv);
	switch (ret_code) {
	case -1:
		/* select error */
		ret = -1;
		break;
	case 0:
		/* select timeout */
	default:
		/* one or more of curl's file descriptors say there's data to read or write*/
		ret = 0;
		break;
	}

	return ret;
}

CURL* CHttpClient::curl_easy_handler(std::string & sUrl, std::string & strPost, CHttpRequestCollect::iterator it, uint32 uiTimeout)
{
	CURL * curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_URL, sUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, m_requestTimeOut*1000.0f);
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strPost.c_str());
	// write function //
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnWriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &((*it)->m_response));
	curl_easy_setopt(curl, CURLOPT_PRIVATE, it);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, m_connectTimeOut*1000.0f);
	return curl;
}

void CHttpClient::update()
{
	boost::mutex::scoped_lock lock(m_FinishRequestMutex);
	CHttpRequestCollect temp = m_FinishRequestCollect;
	m_FinishRequestCollect.clear();
	lock.unlock();

	uint32 s = m_unregisterRequestCollect.size();
	CHttpRequestCollect::iterator it = temp.begin();
	CHttpRequestCollect::iterator itend = temp.end();
	for (; it != itend; ++it) 
	{
		CHttpRequestPtr& req = (*it);
		if (s > 0)
		{
			auto itSet = m_unregisterRequestCollect.find(req->uuid);
			if (itSet != m_unregisterRequestCollect.end())
			{
				m_unregisterRequestCollect.erase(itSet);
				continue;
			}
		}
		if (req->m_state != CURLE_OK)
		{
			xAppliction::getSingleton().addPrintMessage("*Error:HttpRequest error Code=" + Helper::IntToString(req->m_state) + " UUID="+Helper::Long64ToString(req->uuid)+" URL=" + req->m_url + "?" + req->m_postData + "    Response=" + req->m_response, Log_ErrorLevel);
		}
		else
		{
#ifdef TEST_RELEASE
			LogInfo("HttpRequest CallBack, UUID=" + Helper::Long64ToString(req->uuid) + " URL=" + req->m_url + "?" + req->m_postData + "    Response=" + req->m_response);
#endif
		}


		((*it)->m_callBack)(req);
	}
}

bool CHttpClient::unregisterCallBack(LONG64 requestUUID)
{
	m_unregisterRequestCollect.insert(requestUUID);
	return true;
}

//------------------------------------------------------------------------------------

CHttpRequest::CHttpRequest(const std::string& url, const std::string& postData, HttpCallBack cb) : m_url(url), m_postData(postData), m_callBack(cb), m_state(CURL_LAST)
{
	uuid = httpRequestUUID++;
}