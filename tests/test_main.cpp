void run_rga_tests();
void run_serializer_tests();
void run_peer_socket_tests();
void run_peer_manager_tests();
void run_pipeline_tests();
void run_state_sync_tests();
void run_crash_recovery_tests();

int main() {
    run_rga_tests();
    run_serializer_tests();
    run_peer_socket_tests();
    run_peer_manager_tests();
    run_pipeline_tests();
    run_state_sync_tests();
    run_crash_recovery_tests();
    return 0;
}
