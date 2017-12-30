library(ggplot2)
library(gridExtra)
library(reshape2)
library(extrafont)

brewer.palette <- "Set1"
ggplot <- function(...) ggplot2::ggplot(...) + 
  scale_color_brewer(palette=paste(brewer.palette))

setwd("~/ubuntu1404_node1/ndn-tools-dev-vegas/tools/chunks/experiments/100mb_200ms/run-5")

cwnd.aimd <- read.table("cwnd_aimd.txt", header=T)
cwnd.cubic <- read.table("cwnd_cubic.txt", header=T)
cwnd.vegas1 <- read.table("cwnd_vegas1.txt", header=T)
cwnd.vegas2 <- read.table("cwnd_vegas2.txt", header=T)
cwnd.vegas3 <- read.table("cwnd_vegas3.txt", header=T)

g.cwnd <- ggplot() +
  geom_line(data=cwnd.aimd, aes(x=time, y=cwndsize, color='AIMD')) +
  geom_line(data=cwnd.cubic, aes(x=time, y=cwndsize, color='CUBIC')) +
  geom_line(data=cwnd.vegas1, aes(x=time, y=cwndsize, color='VEGAS1')) +
  geom_line(data=cwnd.vegas2, aes(x=time, y=cwndsize, color='VEGAS2')) +
  geom_line(data=cwnd.vegas3, aes(x=time, y=cwndsize, color='VEGAS3')) +
  xlab("Time (s)") + ylab("Cwnd [Pkts]") + labs(color="Legend")

rate.aimd <- read.table("rate_aimd.txt", header=T)
rate.cubic <- read.table("rate_cubic.txt", header=T)
rate.vegas1 <- read.table("rate_vegas1.txt", header=T)
rate.vegas2 <- read.table("rate_vegas2.txt", header=T)
rate.vegas3 <- read.table("rate_vegas3.txt", header=T)

g.rate <- ggplot() +
  geom_line(data=rate.aimd, aes(x=time, y=kbps/1000, color='AIMD')) +
  geom_line(data=rate.cubic, aes(x=time, y=kbps/1000, color='CUBIC')) +
  geom_line(data=rate.vegas1, aes(x=time, y=kbps/1000, color='VEGAS1')) +
  geom_line(data=rate.vegas2, aes(x=time, y=kbps/1000, color='VEGAS2')) +
  geom_line(data=rate.vegas3, aes(x=time, y=kbps/1000, color='VEGAS3')) +
  xlab("Time (s)") + ylab("Rate [Mbit/s]") + labs(color="Legend")

pngwidth <- 7
  
png('cwndrate.png', height=4, width=pngwidth, units="in", res=300)
grid.arrange(g.rate, g.cwnd)
dev.off()
