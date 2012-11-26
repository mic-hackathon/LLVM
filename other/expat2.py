#!/usr/bin/python
# coding: UTF-8
import re, os, sys, json
from xml.etree import ElementTree
from xml.parsers import expat

class Element(object):
    def __init__(self, name, attributes):
        self.name = name
        self.attributes = attributes
        self.cdata = ''
        self.children = []
    def addChild(self, element):
        self.children.append(element)
    def getAttribute(self, key):
        return self.attributes.get(key)
    def getData(self):
        return self.cdata
    def getElements(self, name=""):
        if name:
            return [c for c in self.children if c.name == name]
        else:
            return list(self.children)

class Xml2Obj(object):
    def __init__(self, rootname):
        self.root = []
        self.nodeStack = []
        self.rootname = rootname
    def StartElement(self, name, attributes):
        if name.encode() != self.rootname and len(self.nodeStack) == 0 :
            return
        element = Element(name.encode(), attributes)
        if self.nodeStack:
            parent = self.nodeStack[-1]
            parent.addChild(element)
        else:
            self.root.append(element)
        self.nodeStack.append(element)
    def EndElement(self, name):
        if len(self.nodeStack) != 0:
            self.nodeStack.pop()
    def CharacterData(self, data):
        if [] == self.nodeStack:
            return
        if data.strip():
            data = data.encode()
            element = self.nodeStack[-1]
            element.cdata += data
    def Parse(self, filename):
        Parser = expat.ParserCreate()
        Parser.StartElementHandler = self.StartElement
        Parser.EndElementHandler = self.EndElement
        Parser.CharacterDataHandler = self.CharacterData
        
        ParserStatus = Parser.Parse(open(filename).read(), 1)
        return self.root


if __name__ == "__main__"  :
    print "start"
    parser = Xml2Obj("vm-filesystem")
    root_element = parser.Parse('sample.xml')
    print "end"

