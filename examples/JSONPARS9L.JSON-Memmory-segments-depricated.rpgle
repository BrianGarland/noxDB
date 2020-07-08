       // ------------------------------------------------------------- *
       // noxDB - Not only XML. JSON, SQL and XML made easy for RPG

       // Company . . . : System & Method A/S - Sitemule
       // Design  . . . : Niels Liisberg

       // Unless required by applicable law or agreed to in writing, software
       // distributed under the License is distributed on an "AS IS" BASIS,
       // WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.

       // Look at the header source file "QRPGLEREF" member "NOXDB"
       // for a complete description of the functionality

       // When using noxDB you need two things:
       //  A: Bind you program with "NOXDB" Bind directory
       //  B: Include the noxDB prototypes from QRPGLEREF member NOXDB

       // Parse Strings

       // ------------------------------------------------------------- *
       Ctl-Opt BndDir('NOXDB') dftactgrp(*NO) ACTGRP('QILE');
      /include qrpgleRef,noxdb
       Dcl-S pJson              Pointer;
       Dcl-S pJson2             Pointer;
       Dcl-S pSegment           Pointer;
       Dcl-S pNode              Pointer;
       Dcl-S name               VarChar(64);
       // ------------------------------------------------------------- *
          *inlr = *on;

          // Load an other json into a new memory segment
          pSegment = json_SegmentSelectNo(1);
          pJson2 = json_ParseFile ('/noxdb/json/demo.json');
          pNode = json_Locate(pJson2 : '/o/f');
          name =  json_GetStr(pNode);   // Show john
          json_NodeDelete(pNode);        // Delete john

          // Always dispose the complete Segment
          json_SegmentDispose(pSegment);

          json_memstat();

