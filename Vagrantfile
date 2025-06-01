Vagrant.configure("2") do |config|
  nodes = {
    "server" => "192.168.56.10",
    "client1" => "192.168.56.11",
    "client2" => "192.168.56.12"
  }

  nodes.each do |name, ip|
    config.vm.define name do |node|
      node.vm.box = "bento/ubuntu-22.04"
      node.vm.synced_folder '.', '/vagrant', disabled: true
      node.vm.hostname = name
      node.vm.network "private_network", ip: ip

      node.vm.provision "docker"
      # Download repo
      node.vm.provision "shell", inline: <<-SHELL
        sudo apt update
        sudo apt install -y git
        rm -rf multicast_service
        git clone https://github.com/lpestov/multicast_service.git && rsync -a --delete multicast_service/* .
      SHELL

      # Set up firewall
      node.vm.provision "shell", inline: <<-SHELL
        iptables -A INPUT -p tcp --dport=22 -j ACCEPT
        iptables -A INPUT -i lo -j ACCEPT
        iptables -A INPUT -p icmp -j ACCEPT
        sudo iptables -P INPUT DROP
      SHELL
      if name == "server"
        node.vm.provision "shell", inline: <<-SHELL
          iptables -A INPUT -m state --state ESTABLISHED,RELATED -j ACCEPT
        SHELL
        nodes.each do |node_name, node_ip|
          next if node_name == "server"
          node.vm.provision "shell", inline: <<-SHELL
            iptables -A INPUT -p udp --dport=8080 -s "#{node_ip}" -j ACCEPT
          SHELL
        end
      else
        server_ip = nodes["server"]
        node.vm.provision "shell", inline: <<-SHELL
          iptables -A INPUT -m state --state ESTABLISHED,RELATED ! -s "#{server_ip}" -j ACCEPT
          iptables -A INPUT -p udp -s "#{server_ip}" -j ACCEPT
        SHELL
      end

      # Update DNS
      if name != "server"
        server_ip = nodes["server"]
        node.vm.provision "shell", inline: <<-SHELL
          echo "#{server_ip} server" | sudo tee -a /etc/hosts
        SHELL
      end
      # Build and run containers
      node.vm.provision "docker" do |d|
        if name == "server"
          d.build_image ".", args: "-f server/Dockerfile -t game_server"
          d.run "game_server",
            name: "game_server",
            args: "-it --network host -p 8080:8080/udp"
        else
          d.build_image ".", args: "-f client/Dockerfile -t game_client"
          d.run "game_client",
            name: "client_#{name}",
            args: "-d --network host",
            cmd: "bash -c './client Client_#{name}'"
        end
      end
    end
  end
end
