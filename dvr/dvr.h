#ifdef __cplusplus
extern "C" {
#endif
    int PeerInit();
    int Finalize(int);
    int GetFrames();
    void PublishFrame(int);
#ifdef __cplusplus
}
#endif