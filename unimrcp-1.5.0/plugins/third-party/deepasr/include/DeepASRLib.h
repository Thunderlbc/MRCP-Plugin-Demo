/*
 * Create new Session with Server for Speech Recognization
 * @return const char* - Universal Unique Session ID
 * @param const char *modelName - [in] for determine which model to use for recognizing
 * @param char *errorCode - [out] status code for BeginSession
 */
#ifdef __cplusplus
extern "C" {
#endif
const char *begin_session(const char *modelName, const char *format, int *errorCode);

/*
 * Send Binary/Text data to Server using Specified Session
 * @return bool - 0 stands for success 1 otherwise
 * @param const char *SessionID - [in] Unique SessionID returned by BeginSession
 * @param const void *Data - [in] binary/text data to be send
 * @param int length - [in] length of the data
 * @param int isBinary - [in] whether the data is binary
 * @param char *errorCode - [out] status code for WriteData
 */
bool write_data(const char *SessionID, const char*Data, int Length, int isBinary, int *errorCode);

/*
 * End Session using specified Session and display the ending reasaon(`message`)
 * @return bool - 0 stands for success 1 otherwise
 * @param const char *SessionID - [in] Unique SessionID returned by BeginSession
 * @param const char *message - [in] message for diplaying ending reason
 * @param char *errorCode - [out] status code for EndSession
 */

bool end_session(const char *SessionID, const char *message, int *errorCode);

/*
 * Attempting to get recognization result, return when results are avaiable
 *
 * @return std::string - "" empty string indicates that results are not avaiable, otherwise indicates final results
 * @param std::string SessionID - [in] Unique Session ID for identifying session
 */
const char *get_recog_result(const char *SessionID);

#ifdef __cplusplus
}
#endif
