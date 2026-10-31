int main(void) {
  printf("ready");
  return shared_helper(1) + ghost_fn(2);
}
