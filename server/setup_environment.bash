source load_env.bash

# Update package lists
sudo apt update

# Upgrade installed packages
sudo apt upgrade -y

# Install git (if not already present) and build tools (might need later)
sudo apt install -y git build-essential

# Install python3
sudo apt install python3

# Install pip
python3-pip

# Install pip3 requirements
pip3 install -r pip3_requirements.txt
