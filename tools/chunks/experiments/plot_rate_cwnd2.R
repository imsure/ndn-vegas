library(ggplot2)
library(gridExtra)
library(reshape2)
library(extrafont)

brewer.palette <- "Set1"
ggplot <- function(...) ggplot2::ggplot(...) + 
  scale_color_brewer(palette=paste(brewer.palette))

setwd("~/ubuntu1404_node1/ndn-tools-dev-vegas/tools/chunks/experiments/100mb_200ms_compete/run-3")

cwnd.aimd <- read.table("cwnd_aimd.txt", header=T)
cwnd.cubic <- read.table("cwnd_cubic.txt", header=T)
cwnd.vegas <- read.table("cwnd_vegas.txt", header=T)

g.cwnd <- ggplot() +
  geom_line(data=cwnd.aimd, aes(x=time, y=cwndsize, color='AIMD')) +
  geom_line(data=cwnd.cubic, aes(x=time, y=cwndsize, color='CUBIC')) +
  geom_line(data=cwnd.vegas, aes(x=time, y=cwndsize, color='VEGAS')) +
  xlab("Time (s)") + ylab("Cwnd [Pkts]") + labs(color="Legend")

rate.aimd <- read.table("rate_aimd.txt", header=T)
rate.cubic <- read.table("rate_cubic.txt", header=T)
rate.vegas <- read.table("rate_vegas.txt", header=T)

g.rate <- ggplot() +
  geom_line(data=rate.aimd, aes(x=time, y=kbps/1000, color='AIMD')) +
  geom_line(data=rate.cubic, aes(x=time, y=kbps/1000, color='CUBIC')) +
  geom_line(data=rate.vegas, aes(x=time, y=kbps/1000, color='VEGAS')) +
  xlab("Time (s)") + ylab("Rate [Mbit/s]") + labs(color="Legend")

pngwidth <- 7
  
png('cwndrate.png', height=4, width=pngwidth, units="in", res=300)
grid.arrange(g.rate, g.cwnd)
dev.off()
