<!---
File :- README.md
Description :- Markdown file specifying instructions related to project.
-->

# Version of NPM and Node on which validation was done
>NPM Version
>>11.0.0
>
>Node Version
>>23.4.0

# Execute Cloud Connect Service in Non-Containerize Manner
## *Pre-requisite* -
1. Follow *https://nodejs.org/en/download/package-manager* to install npm and then node on the machine where the Service will be executed.
2. From the folder containing *package.json* execute the command **npm install --production** to install all the required *Node* modules for executing service. 
3. Create a folder *config* at the location where the file *index.js* is present. Within the folder create a file *config.json* similar to *\__test\__/config/config.json*.
4. Update the configuration JSON file *config.json* with required topics and brokers info.
## *Execution* -
- From the folder containing *index.js* execute the command **npm start**.

# Execute Cloud Connect Service Unit and Integration Tests in Non-Containerize Manner
## *Pre-requisite* -
1. Follow *https://nodejs.org/en/download/package-manager* to install npm and then node on the machine where the Service will be executed.
2. From the folder containing *package.json* execute the command **npm install** to install all the required *Node* modules for executing service and unit test cases.
## *Execution* -
- From the folder containing *index.js* execute the command **npm test**
- By default code coverage has been enabled for Unit Tests. In case it needs to be disabled then set the value of *collectCoverage* to false in the file *jest.config.js*.
- Coverage related artifacts are generated in the folder <ROOT_DIR>/coverage.
- Test execution reports are generated in the folder <ROOT_DIR>/reports.
- For Integration Tests (IT) coverage, start the Cloud Connect Service using the command **npx c8 npm start**. 
- After all ITs are executed, stop the Cloud Connect Service using Ctrl + C or kill the requisite process. 
- The IT Coverage report will be automatically generated in the folder <ROOT_DIR>/coverage after Cloud Connect Service is stopped.
- In order to generate combined coverage report of UT and IT, execute the following commands in sequence post UT and IT Code Coverage generation
    >*npx istanbul-merge --out coverage/final/coverage-final.json coverage/unit/coverage-final.json coverage/it/coverage-final.json*
    >
    >*npx nyc report --temp-dir=coverage/final --reporter=text --reporter=html --report-dir=coverage/final* 
