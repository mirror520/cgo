#ifdef __cplusplus
extern "C" {
#endif
    #include <libavformat/avformat.h>
    #include <stdbool.h>

    int PeerInit();
    int Finalize(int);
    int GetChannelNums();
    int GetFrameNums(int);
    int Snapshot(int, AVFrame **);
    void SetPublish(int, bool);
    bool GetPublish(int);
#ifdef __cplusplus
}
#endif
