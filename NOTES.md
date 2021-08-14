[ JvB 2021-08-13 ] Docker image build needs enough RAM, if not build fails by compiler getting killed. 
 Add a swap device if not already: sudo (fallocate -l 1G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile)
 
