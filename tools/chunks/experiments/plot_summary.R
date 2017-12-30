library(ggplot2)
library(gridExtra)
library(reshape2)
library(extrafont)

brewer.palette <- "Set1"
ggplot <- function(...) ggplot2::ggplot(...) + 
  scale_color_brewer(palette=paste(brewer.palette))

setwd("~/ubuntu1404_node1/ndn-tools-dev-vegas/tools/chunks/experiments/100mb_200ms")

summary <- read.table("summary.txt", header=T)

g.duration <- ggplot(data=summary, aes(x=run, y=duration, color=pipeline)) +
    geom_point() + geom_line() + ylab("Time Taken (s)") +
    scale_x_discrete(labels=c('1'='Run #1', '2'='Run #2', '3'='Run #3', '4'='Run #4', '5'='Run #5'))
  # theme(legend.position="none")

g.nRetx <- ggplot(data=summary, aes(x=run, y=nRetx, color=pipeline)) +
  geom_point() + geom_line() + ylab("# of Retransmitted Interest") +
  scale_x_discrete(labels=c('1'='Run #1', '2'='Run #2', '3'='Run #3', '4'='Run #4', '5'='Run #5'))
  # theme(legend.position="none")

g.goodput <- ggplot(data=summary, aes(x=run, y=goodput, color=pipeline)) +
  geom_point() + geom_line() + ylab("Goodput (Mbit/s)") +
  scale_x_discrete(labels=c('1'='Run #1', '2'='Run #2', '3'='Run #3', '4'='Run #4', '5'='Run #5'))

pngwidth <- 14
  
png('summary.png', height=3, width=pngwidth, units="in", res=300)
grid.arrange(g.duration, g.nRetx, g.goodput, ncol=3)
dev.off()
