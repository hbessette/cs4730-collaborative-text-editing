void run_rga_tests();
void run_serializer_tests();
void run_peer_socket_tests();
void run_peer_manager_tests();
void run_pipeline_tests();

int main() {
    run_rga_tests();
    run_serializer_tests();
    run_peer_socket_tests();
    run_peer_manager_tests();
    run_pipeline_tests();
    return 0;
}
