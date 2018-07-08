#ifdef __cplusplus
extern "C" {
#endif
    int PeerInit();
    int Finalize(int);
    int GetFrames(int);
    void PublishFrame(int);
#ifdef __cplusplus
}
#endif