FROM gcc:latest      
WORKDIR /app         
COPY . .             
CMD ["/bin/bash"]   
