void run_rga_tests();
void run_serializer_tests();
void run_peer_socket_tests();

int main() {
    run_rga_tests();
    run_serializer_tests();
    run_peer_socket_tests();
    return 0;
}
